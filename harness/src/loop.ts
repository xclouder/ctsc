import { readFile, mkdir, writeFile, appendFile } from "node:fs/promises";
import { join } from "node:path";

import { buildPrompt, invokeAgent, type AgentRunOptions } from "./agent.js";
import { buildCtsc } from "./build.js";
import { diffPhase } from "./differ.js";
import { getOracle } from "./oracle.js";
import { pickNextFixture } from "./planner.js";
import { runCtsc, findCtscExe } from "./runner.js";
import { ensureFixtureStatus, ensureStateDir, loadProgress, saveProgress } from "./state.js";
import { deferFixture, DEFAULT_WATCHDOG } from "./watchdog.js";
import { FAILURES_FILE, REPORTS_DIR } from "./paths.js";
import type { DiffResult, Fixture, OracleArtifacts, Phase, ProgressState } from "./types.js";

/** Pick the expected artifact for a phase; returns undefined only when the
 * phase has no oracle at all (not when the artifact is an empty string). */
function pickExpected(phase: Phase, o: OracleArtifacts): string | undefined {
  switch (phase) {
    case "scanner": return o.tokensJson;
    case "parser":  return o.astJson;
    case "binder":
    case "checker": return o.tokensJson; // reuses the slot for stub JSON
    case "emitter": return o.emitJs;
    default:        return undefined;
  }
}

export interface LoopOptions {
  maxIterations: number;   /* use Number.POSITIVE_INFINITY for --forever */
  phase?: Phase;
  dry: boolean;            /* skip real ctsc exec and agent invocation */
  build: boolean;          /* run cmake --build between attempts */
  agent: AgentRunOptions;
  /** Run ctsc + diff but don't invoke the agent on a mismatch.
   * Natural-pass fixtures are recorded as passed; failing ones are left
   * as-is (no defer) so a later run with a real agent can retry them. */
  noAgent: boolean;
  /** Clear the deferred flag on all non-passed fixtures before starting. */
  retryDeferred: boolean;
  /** Print cumulative progress every N iterations. 0 disables. */
  statusEvery: number;
}

export const DEFAULT_LOOP: LoopOptions = {
  maxIterations: 5,
  dry: false,
  build: false,
  agent: {},
  noAgent: false,
  retryDeferred: false,
  statusEvery: 10,
};

export async function runLoop(partial: Partial<LoopOptions> = {}): Promise<void> {
  const opts: LoopOptions = { ...DEFAULT_LOOP, ...partial };
  await ensureStateDir();
  await mkdir(REPORTS_DIR, { recursive: true });

  const state = await loadProgress();

  if (opts.retryDeferred) {
    let reset = 0;
    for (const s of Object.values(state.fixtures)) {
      if (s.deferred && !s.passed) {
        s.deferred = false;
        s.noProgressCount = 0;
        reset++;
      }
    }
    if (reset > 0) {
      console.log(`[loop] retry-deferred: reset ${reset} deferred fixtures`);
      await saveProgress(state);
    }
  }

  for (let iter = 0; iter < opts.maxIterations; iter++) {
    const fx = await pickNextFixture(state, { onlyPhase: opts.phase });
    if (!fx) {
      console.log("[loop] curriculum exhausted - all fixtures passed or deferred");
      break;
    }

    const status = ensureFixtureStatus(state, fx);
    status.attempts++;
    status.lastAttempt = new Date().toISOString();

    console.log(`\n[iter ${iter + 1}/${opts.maxIterations}] ${fx.phase}/${fx.stage} :: ${fx.relPath}`);

    const src = await readFile(fx.sourcePath, "utf8");
    const oracle = await getOracle(fx, src);
    const expected = pickExpected(fx.phase, oracle);
    if (expected === undefined) {
      console.warn(`  [skip] no oracle for phase ${fx.phase}`);
      await deferFixture(status, `no oracle for phase ${fx.phase}`);
      await saveProgress(state);
      continue;
    }

    if (opts.build && !opts.dry) {
      const b = await buildCtsc();
      if (!b.ok) {
        console.error(`  [build-fail] ${b.stderr.slice(-800)}`);
        status.lastDiffSummary = `build failed`;
        status.noProgressCount++;
        await saveProgress(state);
        if (status.noProgressCount >= DEFAULT_WATCHDOG.maxNoProgress) {
          await deferFixture(status, "build repeatedly failing");
        }
        continue;
      }
    }

    if (!opts.dry) {
      const exe = await findCtscExe();
      if (!exe) {
        console.warn("  [warn] ctsc not built; re-run bootstrap then add --build.");
        return;
      }
    }

    const run = await runCtsc(fx, { dry: opts.dry });
    const actual = run.stdout;

    if (run.exitCode !== 0) {
      console.error(`  [runner] exit=${run.exitCode} stderr=${run.stderr.slice(0, 400)}`);
    }

    const diff = diffPhase(fx.phase, expected, actual);
    status.lastDiffSummary = diff.summary;

    if (diff.equal) {
      status.passed = true;
      status.noProgressCount = 0;
      console.log(`  [pass] ${diff.summary}`);
      await saveProgress(state);
      await appendFailure(fx, diff, "pass");
      continue;
    }

    console.warn(`  [fail] ${diff.summary}`);
    await appendFailure(fx, diff, "fail");

    const prompt = await buildPrompt(fx, diff, { expected, actual });
    const report = join(REPORTS_DIR, `${Date.now()}_${safeId(fx.id)}.md`);
    await writeFile(report, formatReport(fx, diff, expected, actual), "utf8");

    if (opts.dry) {
      console.log(`  [dry] prompt written (${prompt.length} bytes), agent NOT invoked.`);
      /* In dry mode we just want to smoke-test every fixture, so we mark
       * it deferred immediately to move the planner on. */
      await deferFixture(status, "dry-run: no agent available to close the gap");
      await saveProgress(state);
      continue;
    }

    if (opts.noAgent) {
      /* Survey mode: don't invoke the agent, just move on. The fixture stays
       * in a non-pass / non-defer state so a later run with an authed agent
       * can retry it. Mark it as deferred once so the planner advances past
       * it in this survey run; a later real agent loop can reset it. */
      await deferFixture(status, "survey (no agent)");
      await saveProgress(state);
      await maybePrintStatus(state, iter + 1, opts.statusEvery);
      continue;
    }

    const ar = await invokeAgent(fx, prompt, opts.agent);
    console.log(`  [agent] exit=${ar.exitCode} dur=${ar.durationMs}ms session=${ar.sessionDir}`);
    if (!ar.invoked) {
      console.warn("  [warn] agent not invoked - no progress will be made; aborting loop");
      break;
    }
    if (ar.exitCode !== 0) {
      /* Agent CLI itself failed (missing, not logged in, crashed). Without a
       * working agent we cannot close the gap; treat as a hard no-progress
       * signal and defer so the planner moves on to the next fixture. */
      console.warn(`  [agent] CLI returned non-zero; deferring ${fx.relPath}`);
      await deferFixture(status, `agent exit=${ar.exitCode}`);
      await saveProgress(state);
      continue;
    }

    /* Agent claims success. Rebuild ctsc and re-diff immediately so we capture
     * progress without waiting for the next planner tick and so we can tell a
     * real fix from a no-op edit. */
    let verified = diff;
    if (opts.build) {
      const rb = await buildCtsc();
      if (!rb.ok) {
        console.error(`  [post-agent build-fail] ${rb.stderr.slice(-800)}`);
        status.noProgressCount++;
        if (status.noProgressCount >= DEFAULT_WATCHDOG.maxNoProgress) {
          await deferFixture(status, "post-agent build repeatedly failing");
        }
        await saveProgress(state);
        await maybePrintStatus(state, iter + 1, opts.statusEvery);
        continue;
      }
    }
    const run2 = await runCtsc(fx, { dry: false });
    verified = diffPhase(fx.phase, expected, run2.stdout);
    status.lastDiffSummary = verified.summary;
    if (verified.equal) {
      status.passed = true;
      status.noProgressCount = 0;
      console.log(`  [verified] ${verified.summary}`);
      await appendFailure(fx, verified, "pass");
    } else {
      status.noProgressCount++;
      console.warn(`  [still-fail] ${verified.summary}`);
      if (status.noProgressCount >= DEFAULT_WATCHDOG.maxNoProgress) {
        await deferFixture(status, `no progress after ${status.noProgressCount} attempts`);
      }
    }
    await saveProgress(state);
    await maybePrintStatus(state, iter + 1, opts.statusEvery);
  }
}

async function maybePrintStatus(state: ProgressState, iter: number, every: number): Promise<void> {
  if (!every || iter % every !== 0) return;
  const fs = Object.values(state.fixtures);
  const total = fs.length;
  const passed = fs.filter((f) => f.passed).length;
  const deferred = fs.filter((f) => f.deferred).length;
  const failed = total - passed - deferred;
  console.log(`[status @ iter=${iter}] total=${total} passed=${passed} failed=${failed} deferred=${deferred}`);
}

function formatReport(fx: Fixture, diff: DiffResult, expected: string, actual: string): string {
  return [
    `# ${fx.relPath}`,
    ``,
    `- phase: ${fx.phase}`,
    `- stage: ${fx.stage}`,
    ``,
    `## Diff`,
    ``,
    diff.summary,
    ``,
    `## Expected`,
    "```",
    expected.slice(0, 8000),
    "```",
    ``,
    `## Actual`,
    "```",
    actual.slice(0, 8000),
    "```",
    ``,
  ].join("\n");
}

function safeId(id: string): string {
  return id.replace(/[\\/:*?"<>|]/g, "_").slice(0, 120);
}

async function appendFailure(fx: Fixture, diff: DiffResult, kind: "pass" | "fail"): Promise<void> {
  await mkdir(REPORTS_DIR, { recursive: true });
  await appendFile(FAILURES_FILE, JSON.stringify({ at: new Date().toISOString(), id: fx.id, kind, summary: diff.summary }) + "\n").catch(() => {});
}
