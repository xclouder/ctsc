import { spawn } from "node:child_process";

import { CTSC_DIR, getBuildDir } from "./paths.js";

export interface BuildResult {
  ok: boolean;
  stdout: string;
  stderr: string;
  durationMs: number;
}

export interface BuildOptions {
  /** Absolute build dir. Default: CTSC_BUILD_DIR env or `ctsc/build/default`. */
  buildDir?: string;
  /** Optional preset name (kept for back-compat; ignored when buildDir is explicit). */
  preset?: string;
  /** CMake generator (default Ninja). */
  generator?: string;
  /** CMAKE_C_COMPILER (e.g. 'clang', 'cl'). */
  compiler?: string;
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

/**
 * Build ctsc.
 *
 * Path matrix:
 *   - buildDir explicit        -> use it (bypass preset)
 *   - preset given, no buildDir -> use `cmake --preset <preset>` + build/<preset>
 *   - neither                  -> use getBuildDir() (honours CTSC_BUILD_DIR env)
 *
 * The direct-buildDir path lets per-phase parallel loops each own their own
 * build tree so ninja doesn't fight over the same .ninja_log / object files.
 */
export async function buildCtsc(opts: BuildOptions | string = {}): Promise<BuildResult> {
  if (typeof opts === "string") opts = { preset: opts };

  if (opts.preset && !opts.buildDir && !process.env.CTSC_BUILD_DIR) {
    const cfg = await run("cmake", ["--preset", opts.preset], CTSC_DIR);
    if (!cfg.ok) return cfg;
    return await run("cmake", ["--build", `build/${opts.preset}`], CTSC_DIR);
  }

  const buildDir = opts.buildDir ?? getBuildDir();
  const generator = opts.generator ?? "Ninja";
  const configArgs = ["-S", CTSC_DIR, "-B", buildDir, "-G", generator];
  if (opts.compiler) configArgs.push(`-DCMAKE_C_COMPILER=${opts.compiler}`);
  const cfg = await run("cmake", configArgs, CTSC_DIR);
  if (!cfg.ok) return cfg;
  return await run("cmake", ["--build", buildDir], CTSC_DIR);
}
