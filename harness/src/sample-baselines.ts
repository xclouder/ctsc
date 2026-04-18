/**
 * Sample fixtures from upstream/TypeScript/tests/cases/**.
 *
 * The TS test corpus has tens of thousands of files; we pull small ones that
 * parse cleanly and copy them into fixtures/scanner/from-upstream/ with a
 * difficulty prefix derived from byte size. The harness then sees them as
 * additional scanner fixtures.
 *
 * Usage:
 *   npx tsx src/sample-baselines.ts --phase scanner --count 50
 */

import { mkdir, readdir, readFile, stat, writeFile } from "node:fs/promises";
import { basename, join, relative } from "node:path";

import { FIXTURES_DIR, UPSTREAM_DIR } from "./paths.js";

type Args = { phase: string; count: number; pattern?: string };

function parseArgs(): Args {
  const a: Args = { phase: "scanner", count: 50 };
  const argv = process.argv.slice(2);
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    if (k === "--phase") a.phase = argv[++i];
    else if (k === "--count") a.count = Number(argv[++i]);
    else if (k === "--pattern") a.pattern = argv[++i];
  }
  return a;
}

async function walk(dir: string, out: string[] = []): Promise<string[]> {
  const entries = await readdir(dir, { withFileTypes: true }).catch(() => []);
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) await walk(p, out);
    else out.push(p);
  }
  return out;
}

async function main(): Promise<void> {
  const { phase, count, pattern } = parseArgs();
  const root = join(UPSTREAM_DIR, "tests", "cases", "conformance");
  const all = await walk(root);
  const tsFiles = all.filter((p) => p.endsWith(".ts") && !p.endsWith(".d.ts"));
  const filtered = pattern ? tsFiles.filter((p) => p.includes(pattern)) : tsFiles;

  const sized: { path: string; size: number }[] = [];
  for (const p of filtered) {
    const s = await stat(p);
    if (s.size < 4 * 1024) sized.push({ path: p, size: s.size });
  }
  sized.sort((a, b) => a.size - b.size);

  const target = join(FIXTURES_DIR, phase, "from-upstream");
  await mkdir(target, { recursive: true });
  const picks = sized.slice(0, count);
  let idx = 0;
  for (const p of picks) {
    idx++;
    const diff = difficultyFromSize(p.size);
    const name = `${String(diff).padStart(3, "0")}_${sanitise(basename(p.path))}`;
    const dest = join(target, name);
    const src = await readFile(p.path, "utf8");
    await writeFile(dest, src, "utf8");
  }
  console.log(`sampled ${picks.length} files into ${relative(process.cwd(), target)}`);
}

function difficultyFromSize(bytes: number): number {
  // map 0..4096 bytes to 100..900 difficulty bucket
  return 100 + Math.min(800, Math.floor(bytes / 5));
}

function sanitise(s: string): string {
  return s.replace(/[^A-Za-z0-9._-]/g, "_");
}

main().catch((e) => { console.error(e); process.exit(1); });
