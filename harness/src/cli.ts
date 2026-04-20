#!/usr/bin/env tsx
import { readdir, readFile } from "node:fs/promises";
import { join, resolve } from "node:path";

import { buildPrompt } from "./agent.js";
import { diffPhase } from "./differ.js";
import { runLoop, DEFAULT_LOOP } from "./loop.js";
import { getOracle } from "./oracle.js";
import { STATE_DIR, REPO_ROOT } from "./paths.js";
import { listFixtures } from "./planner.js";
import { runCtsc, findCtscExe } from "./runner.js";
import { runSelfhost } from "./selfhost.js";
import { runProjects } from "./project.js";
import { loadProgress, ensureStateDir } from "./state.js";
import type { FixtureStatus, Phase, ProgressState } from "./types.js";

interface Args2 {
  cmd: string;
  flags: Record<string, string | boolean>;
  multi: Record<string, string[]>;
  rest: string[];
}

function parseArgs(argv: string[]): Args2 {
  const [, , cmd, ...rest] = argv;
  const flags: Record<string, string | boolean> = {};
  const multi: Record<string, string[]> = {};
  const positional: string[] = [];
  const MULTI_KEYS = new Set(["agent-arg"]);
  for (let i = 0; i < rest.length; i++) {
    const a = rest[i];
    if (a.startsWith("--")) {
      const eq = a.indexOf("=");
      const key = eq >= 0 ? a.slice(2, eq) : a.slice(2);
      const val: string | boolean = eq >= 0
        ? a.slice(eq + 1)
        : rest[i + 1] && !rest[i + 1].startsWith("--")
          ? (i++, rest[i])
          : true;
      if (MULTI_KEYS.has(key) && typeof val === "string") {
        (multi[key] ??= []).push(val);
      } else {
        flags[key] = val;
      }
    } else {
      positional.push(a);
    }
  }
  return { cmd: cmd ?? "help", flags, multi, rest: positional };
}

function printHelp(): void {
  console.log(`ctsc-harness

Commands:
  loop [--phase P] [--max N | --forever] [--dry | --no-agent] [--build]
       [--retry-deferred]
       [--build-dir DIR] [--progress-file FILE] [--compiler CC]
       [--agent-cmd X] [--agent-arg X ...] [--model M]
       [--fallback-model M] [--fallback-after N]
       [--timeout SEC] [--status-every N]
    Run the agent loop. --forever runs until all fixtures pass or defer.
    --dry skips ctsc exec and agent invocation (pipeline smoke test).
    --phase isolates this worker to one phase AND auto-assigns its own
      build dir (ctsc/build/phase-<P>) and progress file
      (harness/state/progress.<P>.json), enabling safe parallel workers.
    --build-dir / --progress-file override the auto-derived paths (or
      force isolation for the default single-worker mode).
    Agent defaults: \`agent -p --force --output-format text\` (Cursor CLI).
    Override bin via --agent-cmd or env CTSC_AGENT_CMD.
    Override args via --agent-arg (repeatable) or env CTSC_AGENT_ARGS.
    --fallback-model M  after --fallback-after (default 2) consecutive
      no-progress attempts on a fixture, escalate that fixture to model
      M for the remaining attempts before it gets deferred. Lets you
      run a cheap model as primary and reserve Opus for dead-ends.

  list [--phase P]
    List all fixtures in curriculum order.

  status [--phase P] [--all]
    Print pass/fail summary. Default reads the active progress file
    (progress.json, or CTSC_PROGRESS_FILE). --all aggregates every
    progress*.json under harness/state/ and breaks down by phase.

  diff <fixture-id>
    Show the diff for a single fixture (no agent).

  oracle <fixture-id>
    Print the oracle output (fills cache if missing).

  selfhost [--pkg NAME] [--no-run] [--ctsc-exe PATH]
    M1 smoke test. For each mini-package under harness/selfhost/packages/
    transpile every *.ts with ctsc --emit, diff against ts.transpileModule
    (ES2020), then run the package's runtime.mjs under node to verify the
    emitted JS actually executes end-to-end. --pkg filters by substring.

  project [--project NAME] [--no-run] [--ctsc-exe PATH]
    M3 tsconfig-driven smoke test. For each project under
    harness/selfhost/projects/ load tsconfig.json using ts's own parser,
    enumerate source files honoring include/exclude/files, run
    ctsc --emit per file, stage to the configured outDir, and diff vs
    tsc transpileModule (run per file with parsed compilerOptions).
    Then run runtime.mjs if present.
`);
}

async function main(): Promise<void> {
  const { cmd, flags, multi, rest } = parseArgs(process.argv);
  await ensureStateDir();

  if (cmd === "help" || cmd === "-h" || cmd === "--help") { printHelp(); return; }

  if (cmd === "loop") {
    /* These env vars must be set BEFORE runLoop() imports/uses paths.ts
     * getters so per-phase workers read the right progress file. */
    if (flags["build-dir"]) {
      process.env.CTSC_BUILD_DIR = resolve(REPO_ROOT, String(flags["build-dir"]));
    }
    if (flags["progress-file"]) {
      process.env.CTSC_PROGRESS_FILE = resolve(REPO_ROOT, String(flags["progress-file"]));
    }

    const opts = { ...DEFAULT_LOOP, agent: { ...DEFAULT_LOOP.agent } };
    if (flags.phase)   opts.phase = flags.phase as Phase;
    if (flags.max)     opts.maxIterations = Number(flags.max);
    if (flags.forever) opts.maxIterations = Number.POSITIVE_INFINITY;
    if (flags.dry)       opts.dry = true;
    if (flags.build)     opts.build = true;
    if (flags["no-agent"]) opts.noAgent = true;
    if (flags["retry-deferred"]) opts.retryDeferred = true;
    if (flags["status-every"]) opts.statusEvery = Number(flags["status-every"]);
    if (flags["agent-cmd"])    opts.agent.agentCmd = String(flags["agent-cmd"]);
    if (flags.model)           opts.agent.model = String(flags.model);
    if (flags["fallback-model"]) opts.fallbackModel = String(flags["fallback-model"]);
    if (flags["fallback-after"]) opts.fallbackAfter = Number(flags["fallback-after"]);
    const agentArgs = (multi["agent-arg"] ?? []).filter(Boolean);
    if (agentArgs.length) opts.agent.agentArgs = agentArgs;
    if (flags.timeout) opts.agent.timeoutMs = Number(flags.timeout) * 1000;
    await runLoop(opts);
    return;
  }

  if (cmd === "list") {
    const phase = (flags.phase as Phase | undefined);
    const fs = await listFixtures(phase);
    const state = await loadProgress();
    for (const fx of fs) {
      const s = state.fixtures[fx.id];
      const tag = s?.passed ? "PASS" : s?.deferred ? "DEFER" : s ? "FAIL" : "NEW ";
      console.log(`  ${tag}  ${fx.phase}/${fx.stage}  ${fx.relPath}`);
    }
    return;
  }

  if (cmd === "status") {
    if (flags.phase) {
      process.env.CTSC_PROGRESS_FILE = join(STATE_DIR, `progress.${String(flags.phase)}.json`);
    }
    const wantAll = !!flags.all;
    const states = wantAll ? await loadAllProgressFiles() : [await loadProgress()];

    /* Merge fixtures across all state files. A fixture present in any
     * state counts once; for same-id conflicts (shouldn't happen in
     * per-phase mode) prefer `passed` then `deferred`. */
    const merged = new Map<string, FixtureStatus>();
    for (const s of states) {
      for (const [id, fx] of Object.entries(s.fixtures)) {
        const prev = merged.get(id);
        if (!prev) { merged.set(id, fx); continue; }
        if (fx.passed && !prev.passed) merged.set(id, fx);
        else if (!prev.passed && fx.deferred && !prev.deferred) merged.set(id, fx);
      }
    }
    const all = [...merged.values()];
    const total = all.length;
    const passed = all.filter(f => f.passed).length;
    const deferred = all.filter(f => f.deferred).length;
    const failed = all.filter(f => !f.passed && !f.deferred).length;
    console.log(`total=${total} passed=${passed} failed=${failed} deferred=${deferred}`);

    if (wantAll) {
      const byPhase = new Map<string, { t: number; p: number; f: number; d: number }>();
      for (const fx of all) {
        const p = fx.phase;
        const row = byPhase.get(p) ?? { t: 0, p: 0, f: 0, d: 0 };
        row.t++;
        if (fx.passed) row.p++;
        else if (fx.deferred) row.d++;
        else row.f++;
        byPhase.set(p, row);
      }
      console.log("-- by phase --");
      for (const [ph, r] of [...byPhase.entries()].sort()) {
        const pct = r.t ? ((100 * r.p) / r.t).toFixed(1).padStart(5) : "  -  ";
        console.log(`  ${ph.padEnd(8)} total=${String(r.t).padStart(4)} pass=${String(r.p).padStart(4)} fail=${String(r.f).padStart(4)} defer=${String(r.d).padStart(3)}  (${pct}%)`);
      }
      console.log(`-- files merged: ${states.length} --`);
    } else {
      const s = states[0];
      console.log(`started=${s.startedAt} updated=${s.updatedAt}`);
    }
    return;
  }

  if (cmd === "diff") {
    const id = rest[0];
    if (!id) { console.error("usage: harness diff <fixture-id>"); process.exit(2); }
    const state = await loadProgress();
    const fx = (await listFixtures()).find(f => f.id === id);
    if (!fx) { console.error(`fixture not found: ${id}`); process.exit(2); }
    const src = await readFile(fx.sourcePath, "utf8");
    const oracle = await getOracle(fx, src);
    const expected = oracle.tokensJson ?? oracle.astJson ?? oracle.emitJs ?? "";
    const dry = !!flags.dry;
    const run = await runCtsc(fx, { dry });
    const d = diffPhase(fx.phase, expected, run.stdout);
    console.log(d.summary);
    if (!d.equal && d.firstMismatch) {
      console.log(`  expected: ${d.firstMismatch.expected}`);
      console.log(`  actual:   ${d.firstMismatch.actual}`);
    }
    if (flags.prompt && !d.equal) {
      const prompt = await buildPrompt(fx, d, { expected, actual: run.stdout });
      console.log("\n---PROMPT---\n" + prompt);
    }
    void state;
    return;
  }

  if (cmd === "oracle") {
    const id = rest[0];
    if (!id) { console.error("usage: harness oracle <fixture-id>"); process.exit(2); }
    const fx = (await listFixtures()).find(f => f.id === id);
    if (!fx) { console.error(`fixture not found: ${id}`); process.exit(2); }
    const src = await readFile(fx.sourcePath, "utf8");
    const o = await getOracle(fx, src);
    console.log(o.tokensJson ?? o.astJson ?? "(no oracle)");
    return;
  }

  if (cmd === "selfhost") {
    const rc = await runSelfhost({
      pkg: flags.pkg ? String(flags.pkg) : undefined,
      noRun: !!flags["no-run"],
      ctscExe: flags["ctsc-exe"] ? String(flags["ctsc-exe"]) : undefined,
    });
    process.exit(rc);
  }

  if (cmd === "project") {
    const rc = await runProjects({
      project: flags.project ? String(flags.project) : undefined,
      noRun: !!flags["no-run"],
      ctscExe: flags["ctsc-exe"] ? String(flags["ctsc-exe"]) : undefined,
    });
    process.exit(rc);
  }

  if (cmd === "doctor") {
    const exe = await findCtscExe();
    console.log(`ctsc exe: ${exe ?? "(not found)"}`);
    const state = await loadProgress();
    console.log(`progress fixtures: ${Object.keys(state.fixtures).length}`);
    return;
  }

  printHelp();
  process.exit(2);
}

/** Load every `progress*.json` under harness/state/ (per-phase workers each
 * write their own file). Silently skips unreadable or malformed files so a
 * mid-write transient doesn't break `status --all`. */
async function loadAllProgressFiles(): Promise<ProgressState[]> {
  let names: string[] = [];
  try { names = await readdir(STATE_DIR); } catch { return []; }
  const out: ProgressState[] = [];
  for (const n of names) {
    if (!/^progress(\.[^.]+)?\.json$/.test(n)) continue;
    try {
      const raw = await readFile(join(STATE_DIR, n), "utf8");
      out.push(JSON.parse(raw) as ProgressState);
    } catch { /* mid-write or corrupt; skip */ }
  }
  return out;
}

main().catch((e) => { console.error(e); process.exit(1); });
