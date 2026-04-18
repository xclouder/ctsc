#!/usr/bin/env tsx
import { readFile } from "node:fs/promises";

import { buildPrompt } from "./agent.js";
import { diffPhase } from "./differ.js";
import { runLoop, DEFAULT_LOOP } from "./loop.js";
import { getOracle } from "./oracle.js";
import { listFixtures, pickNextFixture } from "./planner.js";
import { runCtsc, findCtscExe } from "./runner.js";
import { loadProgress, ensureStateDir } from "./state.js";
import type { Phase } from "./types.js";

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
       [--agent-cmd X] [--agent-arg X ...] [--model M]
       [--timeout SEC] [--status-every N]
    Run the agent loop. --forever runs until all fixtures pass or defer.
    --dry skips ctsc exec and agent invocation (pipeline smoke test).
    Agent defaults: \`agent -p --force --output-format text\` (Cursor CLI).
    Override bin via --agent-cmd or env CTSC_AGENT_CMD.
    Override args via --agent-arg (repeatable) or env CTSC_AGENT_ARGS.

  list [--phase P]
    List all fixtures in curriculum order.

  status
    Print pass/fail summary from progress.json.

  diff <fixture-id>
    Show the diff for a single fixture (no agent).

  oracle <fixture-id>
    Print the oracle output (fills cache if missing).
`);
}

async function main(): Promise<void> {
  const { cmd, flags, multi, rest } = parseArgs(process.argv);
  await ensureStateDir();

  if (cmd === "help" || cmd === "-h" || cmd === "--help") { printHelp(); return; }

  if (cmd === "loop") {
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
    const state = await loadProgress();
    const fs = Object.values(state.fixtures);
    const total = fs.length;
    const passed = fs.filter(f => f.passed).length;
    const deferred = fs.filter(f => f.deferred).length;
    const failed = fs.filter(f => !f.passed && !f.deferred).length;
    console.log(`total=${total} passed=${passed} failed=${failed} deferred=${deferred}`);
    console.log(`started=${state.startedAt} updated=${state.updatedAt}`);
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

main().catch((e) => { console.error(e); process.exit(1); });
