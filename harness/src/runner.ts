import { spawn } from "node:child_process";
import { readFile, stat } from "node:fs/promises";

import { getCtscExeCandidates } from "./paths.js";
import type { Fixture, RunResult } from "./types.js";

export interface RunnerOptions {
  /** Non-zero to inject a synthetic runner for dry-run testing. */
  dry?: boolean;
  /** Override ctsc executable discovery. */
  ctscExe?: string;
  /** Kill if runtime exceeds this many ms. */
  timeoutMs?: number;
}

export class RunnerError extends Error {}

export async function findCtscExe(override?: string): Promise<string | null> {
  if (override) {
    try { await stat(override); return override; } catch { return null; }
  }
  for (const p of getCtscExeCandidates()) {
    try { await stat(p); return p; } catch { /* try next */ }
  }
  return null;
}

function runProcess(cmd: string, args: string[], timeoutMs = 30_000): Promise<RunResult> {
  return new Promise((resolve) => {
    const child = spawn(cmd, args, { windowsHide: true });
    let stdout = "";
    let stderr = "";
    const started = Date.now();
    const timer = setTimeout(() => {
      try { child.kill("SIGKILL"); } catch { /* ignore */ }
    }, timeoutMs);
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (c: string) => { stdout += c; });
    child.stderr.on("data", (c: string) => { stderr += c; });
    child.on("close", (code) => {
      clearTimeout(timer);
      resolve({
        exitCode: code ?? -1,
        stdout,
        stderr,
        durationMs: Date.now() - started,
      });
    });
    child.on("error", (err) => {
      clearTimeout(timer);
      resolve({
        exitCode: -1,
        stdout,
        stderr: stderr + `\n[spawn error] ${err.message}`,
        durationMs: Date.now() - started,
      });
    });
  });
}

export async function runCtsc(fx: Fixture, opts: RunnerOptions = {}): Promise<RunResult> {
  if (opts.dry) {
    const src = await readFile(fx.sourcePath, "utf8");
    if (fx.phase === "parser") {
      const empty = JSON.stringify({ kind: "SourceFile", pos: 0, end: src.length, statements: [] });
      return { exitCode: 0, stdout: empty, stderr: "", durationMs: 0 };
    }
    const empty = JSON.stringify({ tokens: [{ kind: "EndOfFileToken", start: 0, end: src.length }], diagnostics: [] });
    return { exitCode: 0, stdout: empty, stderr: "", durationMs: 0 };
  }
  const exe = await findCtscExe(opts.ctscExe);
  if (!exe) {
    return {
      exitCode: -2,
      stdout: "",
      stderr: "ctsc executable not found. Build ctsc first (cmake --preset default && cmake --build ctsc/build/default) or pass --dry.",
      durationMs: 0,
    };
  }
  switch (fx.phase) {
    case "scanner":
      return runProcess(exe, ["--dump-tokens", fx.sourcePath], opts.timeoutMs);
    case "parser":
      return runProcess(exe, ["--dump-ast", fx.sourcePath], opts.timeoutMs);
    case "binder":
      return runProcess(exe, ["--dump-bindings", fx.sourcePath], opts.timeoutMs);
    case "checker": {
      /* Channel decides which CLI command we call; oracle picks matching JSON. */
      const ch = fx.checkerChannel ?? "types";
      if (ch === "diag") return runProcess(exe, ["--check", fx.sourcePath], opts.timeoutMs);
      return runProcess(exe, ["--dump-types", fx.sourcePath], opts.timeoutMs);
    }
    case "emitter":
      return runProcess(exe, ["--emit", fx.sourcePath], opts.timeoutMs);
    default:
      return {
        exitCode: -3,
        stdout: "",
        stderr: `runner: phase '${fx.phase}' is not wired yet`,
        durationMs: 0,
      };
  }
}
