import { readdir, stat } from "node:fs/promises";
import { join, relative, basename } from "node:path";

import { FIXTURES_DIR } from "./paths.js";
import type { Fixture, Phase } from "./types.js";

const PHASE_FILES: Record<Phase, string[]> = {
  scanner: [".ts", ".tsx"],
  parser:  [".ts", ".tsx"],
  binder:  [".ts", ".tsx"],
  checker: [".ts", ".tsx"],
  emitter: [".ts", ".tsx"],
  cli:     [".ts", ".tsx"],
};

const PHASE_ORDER: Phase[] = ["scanner", "parser", "binder", "checker", "emitter", "cli"];

async function walk(dir: string): Promise<string[]> {
  const out: string[] = [];
  const entries = await readdir(dir, { withFileTypes: true }).catch(() => []);
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...await walk(p));
    else out.push(p);
  }
  return out;
}

export async function loadCurriculum(onlyPhase?: Phase): Promise<Fixture[]> {
  const out: Fixture[] = [];
  for (const phase of PHASE_ORDER) {
    if (onlyPhase && phase !== onlyPhase) continue;
    const root = join(FIXTURES_DIR, phase);
    let files: string[] = [];
    try { await stat(root); files = await walk(root); } catch { /* empty */ }
    const exts = PHASE_FILES[phase];
    for (const f of files) {
      if (!exts.some((e) => f.endsWith(e))) continue;
      const rel = relative(FIXTURES_DIR, f).replace(/\\/g, "/");
      const parts = rel.split("/");
      const stage = parts.length >= 3 ? parts[1] : "default";
      out.push({
        id: rel,
        phase,
        stage,
        sourcePath: f,
        relPath: rel,
        difficulty: difficultyHint(basename(f)),
      });
    }
  }
  return out;
}

function difficultyHint(name: string): number {
  const m = /^(\d+)_/.exec(name);
  if (m) return Number(m[1]);
  return 100;
}

export function sortCurriculum(fs: Fixture[]): Fixture[] {
  /* Difficulty-first so the planner interleaves phases: a newly added
   * `binder/top-level/01_*` (difficulty=101) will be picked before a
   * gnarly `scanner/from-upstream/150_*` (difficulty=150), even though
   * scanner sits earlier in PHASE_ORDER. This keeps the agent unblocked
   * when one phase has a handful of very hard fixtures left over. */
  return [...fs].sort((a, b) => {
    if (a.difficulty !== b.difficulty) return a.difficulty - b.difficulty;
    const pi = PHASE_ORDER.indexOf(a.phase) - PHASE_ORDER.indexOf(b.phase);
    if (pi !== 0) return pi;
    return a.relPath.localeCompare(b.relPath);
  });
}
