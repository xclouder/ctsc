/**
 * tsconfig-driven project runner (milestone M3 foundation).
 *
 * For each project under harness/selfhost/projects/<name>/:
 *   1. Load tsconfig.json using ts's own parser. This gives us:
 *      - parsed.fileNames: list of source files honoring include/exclude/files
 *      - parsed.options:   compilerOptions (target, outDir, rootDir, module, ...)
 *   2. For every source file:
 *      a. run `ctsc --emit <file>` and stage to <outDir>/<relativePath>.js
 *      b. run `ts.transpileModule(src, parsed.options)` and stage to a
 *         shadow oracle dir (<outDir>-tsc/...)
 *      c. diff the two output files.
 *   3. Write a `package.json` with `{"type":"module"}` next to outDir so
 *      node can resolve relative ESM imports between compiled files.
 *   4. If runtime.mjs exists, execute it against the ctsc outDir.
 *
 * This runner does NOT yet invoke `ctsc --project <dir>` because the C
 * driver has no such flag. It is the reference specification that the
 * C side will match once `ctsc --project` lands. The harness already
 * validates ctsc end-to-end on real tsconfig-organized projects.
 *
 * Usage:
 *   npm run project                       # run every project
 *   npm run project -- --project p01_simple
 *   npm run project -- --no-run
 *   npm run project -- --ctsc-exe PATH
 */

import { execFile, spawn } from "node:child_process";
import { readdir, readFile, writeFile, mkdir, rm, stat } from "node:fs/promises";
import { dirname, join, relative, resolve, basename } from "node:path";
import { promisify } from "node:util";

import { findCtscExe } from "./runner.js";
import { HARNESS_DIR } from "./paths.js";

const execFileAsync = promisify(execFile);

const SELFHOST_DIR = resolve(HARNESS_DIR, "selfhost");
const PROJECTS_DIR = resolve(SELFHOST_DIR, "projects");

export interface ProjectOptions {
  project?: string;     // substring match against project dir name
  noRun?: boolean;      // skip node runtime.mjs step
  ctscExe?: string;     // explicit ctsc binary
}

interface ProjectFile {
  src: string;          // absolute source path
  rel: string;          // path relative to project root, posix
  outRel: string;       // path inside outDir (e.g. "index.js")
  match: boolean;
  ctscBytes: number;
  tscBytes: number;
  ctscExit: number;
  ctscStderr: string;
  diffPreview?: string;
}

interface ProjectResult {
  project: string;
  tsconfigPath: string;
  rootDir: string;
  outDir: string;
  files: ProjectFile[];
  allMatch: boolean;
  ran: boolean;
  runOk?: boolean;
  runOutput?: string;
  runError?: string;
}

async function listProjects(): Promise<string[]> {
  try {
    const entries = await readdir(PROJECTS_DIR, { withFileTypes: true });
    return entries
      .filter((e) => e.isDirectory())
      .map((e) => e.name)
      .sort();
  } catch {
    return [];
  }
}

async function runCtscEmit(
  ctscExe: string,
  src: string,
  timeoutMs = 30_000,
): Promise<{ exitCode: number; stdout: string; stderr: string }> {
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

function diffPreview(a: string, b: string, maxLines = 20): string {
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

async function runProject(
  ctscExe: string,
  projectDir: string,
  opts: ProjectOptions,
): Promise<ProjectResult> {
  const ts = (await import("typescript")).default;

  const projectName = basename(projectDir);
  const tsconfigPath = join(projectDir, "tsconfig.json");

  const configText = await readFile(tsconfigPath, "utf8");
  const parsedJson = ts.parseConfigFileTextToJson(tsconfigPath, configText);
  if (parsedJson.error) {
    throw new Error(`tsconfig parse error: ${ts.flattenDiagnosticMessageText(parsedJson.error.messageText, "\n")}`);
  }

  // parseJsonConfigFileContent applies include/exclude/files and resolves
  // rootDir/outDir into absolute paths on parsed.options.
  const parsed = ts.parseJsonConfigFileContent(
    parsedJson.config,
    ts.sys,
    projectDir,
  );
  if (parsed.errors.length > 0) {
    const msg = parsed.errors.map((e) => ts.flattenDiagnosticMessageText(e.messageText, "\n")).join("\n");
    throw new Error(`tsconfig resolve error: ${msg}`);
  }

  const rootDir = parsed.options.rootDir ?? join(projectDir, "src");
  const outDir = parsed.options.outDir ?? join(projectDir, "dist");

  await rm(outDir, { recursive: true, force: true });
  await mkdir(outDir, { recursive: true });
  // ESM marker so node resolves relative `.js` imports between compiled files.
  await writeFile(join(outDir, "package.json"), `{"type":"module"}\n`, "utf8");

  // Drop type-check results; we only use the file list and options.
  const sources = parsed.fileNames.filter((f) => !f.endsWith(".d.ts")).sort();

  const files: ProjectFile[] = [];
  let allMatch = true;

  for (const src of sources) {
    const rel = relative(projectDir, src).replaceAll("\\", "/");
    // outRel = path relative to rootDir, with .ts(x) -> .js
    const underRoot = relative(rootDir, src).replaceAll("\\", "/");
    const outRel = underRoot.replace(/\.tsx?$/, ".js");
    const destPath = join(outDir, outRel);

    const r = await runCtscEmit(ctscExe, src);
    const ctscOut = r.stdout;
    const ctscExit = r.exitCode;
    const ctscStderr = r.stderr;

    let tscOut = "";
    try {
      const text = await readFile(src, "utf8");
      const transpile = ts.transpileModule(text, { compilerOptions: parsed.options });
      tscOut = transpile.outputText;
    } catch (e: any) {
      tscOut = `<<tsc error: ${e?.message ?? e}>>`;
    }

    const match = ctscExit === 0 && ctscOut === tscOut;
    if (!match) allMatch = false;

    // Always stage whatever ctsc emitted (truthful runtime probe);
    // fall back to tsc output only if ctsc exit !=0.
    const bytes = ctscExit === 0 && ctscOut ? ctscOut : tscOut;
    await mkdir(dirname(destPath), { recursive: true });
    await writeFile(destPath, bytes, "utf8");

    files.push({
      src,
      rel,
      outRel,
      match,
      ctscExit,
      ctscStderr: ctscStderr.slice(0, 400),
      ctscBytes: Buffer.byteLength(ctscOut, "utf8"),
      tscBytes: Buffer.byteLength(tscOut, "utf8"),
      diffPreview: match ? undefined : diffPreview(ctscOut, tscOut),
    });
  }

  const result: ProjectResult = {
    project: projectName,
    tsconfigPath,
    rootDir,
    outDir,
    files,
    allMatch,
    ran: false,
  };

  const runtime = join(projectDir, "runtime.mjs");
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
      result.runError = ((e?.stdout?.toString?.() ?? "") + "\n" + (e?.stderr?.toString?.() ?? "") + "\n" + (e?.message ?? "")).trim().slice(0, 800);
    }
  }

  return result;
}

function printReport(results: ProjectResult[]): void {
  console.log("\n=== project report ===");
  const col = (s: string, n: number) => s.padEnd(n);

  let totalFiles = 0, matched = 0;
  let ran = 0, runOk = 0;

  for (const r of results) {
    totalFiles += r.files.length;
    matched   += r.files.filter((f) => f.match).length;
    if (r.ran) ran++;
    if (r.runOk) runOk++;

    const byteTag = r.allMatch ? "bytes=MATCH" : "bytes=DIFF ";
    const runTag = !r.ran ? "run=skip   "
                 : r.runOk ? "run=OK     "
                 : "run=FAIL   ";
    console.log(`  ${col(r.project, 22)} files=${r.files.length}  ${byteTag}  ${runTag}  outDir=${relative(PROJECTS_DIR, r.outDir).replaceAll("\\", "/")}`);

    for (const f of r.files) {
      if (!f.match) {
        console.log(`      [diff] ${f.rel}  ctsc=${f.ctscBytes}B  tsc=${f.tscBytes}B  exit=${f.ctscExit}`);
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
  console.log(`  projects: ${results.filter((r) => r.allMatch).length}/${results.length} all files match`);
  console.log(`  runtime:  ${runOk}/${ran} executed successfully under node`);
}

export async function runProjects(opts: ProjectOptions = {}): Promise<number> {
  const ctscExe = await findCtscExe(opts.ctscExe);
  if (!ctscExe) {
    console.error("ctsc executable not found. Build ctsc first (cmake --build ctsc/build/default) or pass --ctsc-exe.");
    return 2;
  }
  console.log(`[project] ctsc=${ctscExe}`);

  const all = await listProjects();
  if (all.length === 0) {
    console.error(`[project] no projects under ${PROJECTS_DIR}`);
    return 2;
  }
  const picked = opts.project ? all.filter((p) => p.includes(opts.project as string)) : all;
  if (picked.length === 0) {
    console.error(`[project] no projects match --project ${opts.project}`);
    return 2;
  }

  const results: ProjectResult[] = [];
  for (const p of picked) {
    const res = await runProject(ctscExe, join(PROJECTS_DIR, p), opts);
    results.push(res);
  }
  printReport(results);

  const anyFail = results.some((r) => !r.allMatch || (r.ran && !r.runOk));
  return anyFail ? 1 : 0;
}
