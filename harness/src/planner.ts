import { loadCurriculum, sortCurriculum } from "./curriculum.js";
import type { Fixture, Phase, ProgressState } from "./types.js";

export interface PickOptions {
  onlyPhase?: Phase;
  retryFailed?: boolean;
}

export async function pickNextFixture(state: ProgressState, opts: PickOptions = {}): Promise<Fixture | null> {
  const fixtures = sortCurriculum(await loadCurriculum(opts.onlyPhase));
  for (const fx of fixtures) {
    const s = state.fixtures[fx.id];
    if (!s) return fx;
    if (s.deferred) continue;
    if (s.passed) continue;
    return fx;
  }
  return null;
}

export async function listFixtures(onlyPhase?: Phase): Promise<Fixture[]> {
  return sortCurriculum(await loadCurriculum(onlyPhase));
}
