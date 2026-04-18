import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

import { PROGRESS_FILE, STATE_DIR } from "./paths.js";
import type { Fixture, FixtureStatus, ProgressState } from "./types.js";

export async function loadProgress(): Promise<ProgressState> {
  try {
    const raw = await readFile(PROGRESS_FILE, "utf8");
    return JSON.parse(raw) as ProgressState;
  } catch {
    return {
      schemaVersion: 1,
      startedAt: new Date().toISOString(),
      updatedAt: new Date().toISOString(),
      fixtures: {},
    };
  }
}

export async function saveProgress(state: ProgressState): Promise<void> {
  state.updatedAt = new Date().toISOString();
  await mkdir(dirname(PROGRESS_FILE), { recursive: true });
  await writeFile(PROGRESS_FILE, JSON.stringify(state, null, 2), "utf8");
}

export function ensureFixtureStatus(state: ProgressState, fx: Fixture): FixtureStatus {
  const existing = state.fixtures[fx.id];
  if (existing) return existing;
  const s: FixtureStatus = {
    id: fx.id,
    phase: fx.phase,
    stage: fx.stage,
    passed: false,
    attempts: 0,
    noProgressCount: 0,
  };
  state.fixtures[fx.id] = s;
  return s;
}

export async function ensureStateDir(): Promise<void> {
  await mkdir(STATE_DIR, { recursive: true });
}
