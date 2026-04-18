import { spawn } from "node:child_process";

import { CTSC_DIR } from "./paths.js";

export interface BuildResult {
  ok: boolean;
  stdout: string;
  stderr: string;
  durationMs: number;
}

function run(cmd: string, args: string[], cwd: string): Promise<BuildResult> {
  return new Promise((resolve) => {
    const child = spawn(cmd, args, { cwd, shell: process.platform === "win32", windowsHide: true });
    let stdout = "";
    let stderr = "";
    const started = Date.now();
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (c: string) => { stdout += c; });
    child.stderr.on("data", (c: string) => { stderr += c; });
    child.on("close", (code) => {
      resolve({ ok: code === 0, stdout, stderr, durationMs: Date.now() - started });
    });
    child.on("error", (err) => {
      resolve({ ok: false, stdout, stderr: stderr + `\n[spawn error] ${err.message}`, durationMs: Date.now() - started });
    });
  });
}

export async function buildCtsc(preset = "default"): Promise<BuildResult> {
  const cfg = await run("cmake", ["--preset", preset], CTSC_DIR);
  if (!cfg.ok) return cfg;
  return await run("cmake", ["--build", `build/${preset}`], CTSC_DIR);
}
