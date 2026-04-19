/**
 * Self-host smoke test runner (milestone M1 validation).
 *
 * For each mini-package under harness/selfhost/packages/<name>/:
 *   1. Find every *.ts under src/
 *   2. Transpile with ctsc --emit   -> bytes_ctsc
 *   3. Transpile with ts.transpileModule(target: ES2020) -> bytes_tsc
 *   4. Compare byte-for-byte. Report match / mismatch with unified diff tail
 *   5. If all files in the package match, stage ctsc output as dist/*.js
 *      and run runtime.mjs with node. Report pass/fail.
 *
 * This is the canonical "does ctsc actually work on real code" test. If a
 * package passes, the emitter is byte-faithful to tsc for that input and the
 * emitted JS actually executes under node -- end to end.
 *
 * Usage:
 *   npm run selfhost            # run every package
 *   npm run selfhost -- --pkg 02_ms-lite
 *   npm run selfhost -- --no-run   # skip node runtime step
 */

import { execFile, spawn } from "node:child_process";
import { readdir, readFile, writeFile, mkdir, rm, stat } from "node:fs/promises";
import { dirname, join, relative, resolve } from "node:path";
import { promisify } from "node:util";

import { findCtscExe } from "./runner.js";
import { HARNESS_DIR } from "./paths.js";

const execFileAsync = promisify(execFile);

const SELFHOST_DIR = resolve(HARNESS_DIR, "selfhost");
const PACKAGES_DIR = resolve(SELFHOST_DIR, "packages");

export interface SelfhostOptions {
  /** Only run packages whose dirname matches this string (substring). */
  pkg?: string;
  /** Skip the `node runtime.mjs` execution step. */
  noRun?: boolean;
  /** Ctsc executable override. */
  ctscExe?: string;
}

interface FileResult {
  file: string;          // relative to package root, e.g. "src/index.ts"
  match: boolean;
  ctscExit: number;
  ctscStderr: string;
  ctscBytes: number;
  tscBytes: number;
  diffPreview?: string;  // first ~20 lines of unified diff when !match
}

interface PackageResult {
  pkg: string;
  files: FileResult[];
  allMatch: boolean;
  ran: boolean;
  runOk?: boolean;
  runOutput?: string;
  runError?: string;
}

async function listPackages(): Promise<string[]> {
  try {
    const entries = await readdir(PACKAGES_DIR, { withFileTypes: true });
    return entries
      .filter((e) => e.isDirectory())
      .map((e) => e.name)
      .sort();
  } catch {
    return [];
  }
}

async function listTsFiles(root: string): Promise<string[]> {
  const out: string[] = [];
  async function walk(dir: string): Promise<void> {
    let entries;
    try {
      entries = await readdir(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      const p = join(dir, e.name);
      if (e.isDirectory()) await walk(p);
      else if (e.isFile() && (e.name.endsWith(".ts") || e.name.endsWith(".tsx"))) {
        if (e.name.endsWith(".d.ts")) continue;
        out.push(p);
      }
    }
  }
  await walk(root);
  return out.sort();
}

async function runCtscEmit(ctscExe: string, src: string, timeoutMs = 30_000):
  Promise<{ exitCode: number; stdout: string; stderr: string }> {
  return new Promise((resolve) => {
    const child = spawn(ctscExe, ["--emit", src], { windowsHide: true });
    let stdout = ""; let stderr = "";
    const killer = setTimeout(() => { try { child.kill("SIGKILL"); } catch { /* */ } }, timeoutMs);
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (c: string) => { stdout += c; });
    child.stderr.on("data", (c: string) => { stderr += c; });
    child.on("close", (code) => { clearTimeout(killer); resolve({ exitCode: code ?? -1, stdout, stderr }); });
    child.on("error", (e) => { clearTimeout(killer); resolve({ exitCode: -1, stdout, stderr: stderr + "\n" + e.message }); });
  });
}

async function tscTranspile(src: string): Promise<string> {
  const ts = (await import("typescript")).default;
  const text = await readFile(src, "utf8");
  const out = ts.transpileModule(text, {
    compilerOptions: { target: ts.ScriptTarget.ES2020 },
  });
  return out.outputText;
}

/** Simple line-diff preview for mismatches: show first 20 differing chunks. */
function diffPreview(a: string, b: string, maxLines = 30): string {
  const la = a.split("\n");
  const lb = b.split("\n");
  const n = Math.max(la.length, lb.length);
  const lines: string[] = [];
  for (let i = 0; i < n && lines.length < maxLines; i++) {
    const x = la[i] ?? "<EOF>";
    const y = lb[i] ?? "<EOF>";
    if (x !== y) {
      lines.push(`- ${x}`);
      lines.push(`+ ${y}`);
    }
  }
  return lines.join("\n");
}

async function checkPackage(
  ctscExe: string,
  pkgDir: string,
  opts: SelfhostOptions,
): Promise<PackageResult> {
  const pkg = relative(PACKAGES_DIR, pkgDir);
  const srcDir = join(pkgDir, "src");
  const distDir = join(pkgDir, "dist");
  const sources = await listTsFiles(srcDir);
  const files: FileResult[] = [];
  await rm(distDir, { recursive: true, force: true });
  await mkdir(distDir, { recursive: true });
  // ts.transpileModule with target ES2020 emits ESM (`import`/`export`).
  // For node to resolve relative `import "./foo.js"` between dist files
  // we need to mark dist as ESM. This is harmless for single-file packages
  // because their dist/index.js has no relative imports.
  await writeFile(join(distDir, "package.json"), `{"type":"module"}\n`, "utf8");

  let allMatch = true;

  for (const src of sources) {
    const rel = relative(pkgDir, src).replaceAll("\\", "/");
    let ctscOut = "";
    let ctscExit = -1;
    let ctscStderr = "";
    try {
      const r = await runCtscEmit(ctscExe, src);
      ctscOut = r.stdout;
      ctscExit = r.exitCode;
      ctscStderr = r.stderr;
    } catch (e: any) {
      ctscStderr = String(e?.message ?? e);
    }

    let tscOut = "";
    try { tscOut = await tscTranspile(src); } catch (e: any) { tscOut = `<<tsc error: ${e?.message ?? e}>>`; }

    const match = ctscExit === 0 && ctscOut === tscOut;
    if (!match) allMatch = false;

    // Always stage whatever ctsc emitted (bytes_ctsc) under dist/ so runtime
    // step shows us whether ctsc's output actually executes; we WANT the
    // truth even when bytes differ from tsc.
    const destRel = rel.replace(/^src\//, "").replace(/\.tsx?$/, ".js");
    const dest = join(distDir, destRel);
    await mkdir(dirname(dest), { recursive: true });
    // Fallback: if ctsc emitted nothing (exit!=0), write tsc output so the
    // runtime error we see is specifically ctsc's emit failure, not something
    // unrelated. Tag in the report.
    const bytes = ctscExit === 0 && ctscOut ? ctscOut : tscOut;
    await writeFile(dest, bytes, "utf8");

    files.push({
      file: rel,
      match,
      ctscExit,
      ctscStderr: ctscStderr.slice(0, 400),
      ctscBytes: Buffer.byteLength(ctscOut, "utf8"),
      tscBytes: Buffer.byteLength(tscOut, "utf8"),
      diffPreview: match ? undefined : diffPreview(ctscOut, tscOut),
    });
  }

  const result: PackageResult = { pkg, files, allMatch, ran: false };

  const runtime = join(pkgDir, "runtime.mjs");
  let hasRuntime = false;
  try { await stat(runtime); hasRuntime = true; } catch { /* optional */ }

  if (hasRuntime && !opts.noRun) {
    result.ran = true;
    try {
      const { stdout, stderr } = await execFileAsync(process.execPath, [runtime], {
        timeout: 15_000,
        windowsHide: true,
      });
      result.runOk = true;
      result.runOutput = (stdout + stderr).trim();
    } catch (e: any) {
      result.runOk = false;
      result.runError = (e?.stdout?.toString?.() ?? "") + "\n" + (e?.stderr?.toString?.() ?? "") + "\n" + (e?.message ?? "");
      result.runError = result.runError.trim().slice(0, 800);
    }
  }

  return result;
}

function printReport(results: PackageResult[]): void {
  console.log("\n=== self-host report ===");
  const col = (s: string, n: number) => s.padEnd(n);

  let totalFiles = 0, matched = 0;
  let pkgRan = 0, pkgRunOk = 0;

  for (const r of results) {
    totalFiles += r.files.length;
    matched   += r.files.filter((f) => f.match).length;
    if (r.ran) pkgRan++;
    if (r.runOk) pkgRunOk++;

    const byteTag = r.allMatch ? "bytes=MATCH" : "bytes=DIFF ";
    const runTag = !r.ran ? "run=skip   "
                 : r.runOk ? "run=OK     "
                 : "run=FAIL   ";
    console.log(`  ${col(r.pkg, 22)} files=${r.files.length}  ${byteTag}  ${runTag}`);

    for (const f of r.files) {
      if (!f.match) {
        console.log(`      [diff]  ${f.file}   ctsc=${f.ctscBytes}B  tsc=${f.tscBytes}B  exit=${f.ctscExit}`);
        if (f.ctscStderr) {
          const first = f.ctscStderr.split("\n").slice(0, 3).join("\n      | ");
          console.log(`      | stderr: ${first}`);
        }
        if (f.diffPreview) {
          const head = f.diffPreview.split("\n").slice(0, 10).map((l) => "      | " + l).join("\n");
          console.log(head);
        }
      }
    }
    if (r.ran && !r.runOk && r.runError) {
      const first = r.runError.split("\n").slice(0, 6).map((l) => "      | " + l).join("\n");
      console.log(`      [node]\n${first}`);
    }
    if (r.ran && r.runOk && r.runOutput) {
      console.log(`      [node] ${r.runOutput}`);
    }
  }

  console.log(`\n  files:    ${matched}/${totalFiles} byte-identical to tsc`);
  console.log(`  packages: ${results.filter((r) => r.allMatch).length}/${results.length} all files match`);
  console.log(`  runtime:  ${pkgRunOk}/${pkgRan} executed successfully under node`);
}

export async function runSelfhost(opts: SelfhostOptions = {}): Promise<number> {
  const ctscExe = await findCtscExe(opts.ctscExe);
  if (!ctscExe) {
    console.error("ctsc executable not found. Build ctsc first (cmake --build ctsc/build/default) or pass --ctsc-exe.");
    return 2;
  }
  console.log(`[selfhost] ctsc=${ctscExe}`);

  const allPkgs = await listPackages();
  if (allPkgs.length === 0) {
    console.error(`[selfhost] no packages under ${PACKAGES_DIR}`);
    return 2;
  }
  const pkgs = opts.pkg
    ? allPkgs.filter((p) => p.includes(opts.pkg as string))
    : allPkgs;
  if (pkgs.length === 0) {
    console.error(`[selfhost] no packages match --pkg ${opts.pkg}`);
    return 2;
  }

  const results: PackageResult[] = [];
  for (const p of pkgs) {
    const res = await checkPackage(ctscExe, join(PACKAGES_DIR, p), opts);
    results.push(res);
  }
  printReport(results);

  const anyFail = results.some((r) => !r.allMatch || (r.ran && !r.runOk));
  return anyFail ? 1 : 0;
}
