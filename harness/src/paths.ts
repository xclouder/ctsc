import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

export const HARNESS_DIR = resolve(__dirname, "..");
export const REPO_ROOT = resolve(HARNESS_DIR, "..");
export const UPSTREAM_DIR = resolve(REPO_ROOT, "upstream", "TypeScript");
export const CTSC_DIR = resolve(REPO_ROOT, "ctsc");
export const FIXTURES_DIR = resolve(REPO_ROOT, "fixtures");
export const STATE_DIR = resolve(HARNESS_DIR, "state");
export const ORACLE_CACHE_DIR = resolve(STATE_DIR, "oracle");
export const REPORTS_DIR = resolve(HARNESS_DIR, "reports");
export const AGENT_SESSIONS_DIR = resolve(STATE_DIR, "agent-sessions");

export const PROGRESS_FILE = resolve(STATE_DIR, "progress.json");
export const FAILURES_FILE = resolve(STATE_DIR, "failures.jsonl");
export const DEFERRED_FILE = resolve(STATE_DIR, "deferred.jsonl");

export const BUILD_DIR = resolve(CTSC_DIR, "build", "default");
export const CTSC_EXE_CANDIDATES = [
  resolve(BUILD_DIR, "ctsc.exe"),
  resolve(BUILD_DIR, "ctsc"),
  resolve(CTSC_DIR, "build", "msvc", "ctsc.exe"),
  resolve(CTSC_DIR, "build", "clang", "ctsc.exe"),
  resolve(CTSC_DIR, "build", "release", "ctsc.exe"),
];

/* ------------------------------------------------------------------ *
 * Env-aware getters.
 *
 * When the harness loop runs in per-phase parallel mode, each worker
 * needs its own progress file and its own ctsc build dir so that 5
 * concurrent loops don't step on each other. The loop sets these env
 * vars BEFORE any other harness module reads them, and the getters
 * below honour the overrides.
 * ------------------------------------------------------------------ */

/** Path to the progress file this worker should read/write.
 *  Override via env CTSC_PROGRESS_FILE (absolute or repo-relative). */
export function getProgressFile(): string {
  return process.env.CTSC_PROGRESS_FILE
    ? resolve(REPO_ROOT, process.env.CTSC_PROGRESS_FILE)
    : PROGRESS_FILE;
}

/** Ctsc build dir this worker uses (cmake -B target).
 *  Override via env CTSC_BUILD_DIR. */
export function getBuildDir(): string {
  return process.env.CTSC_BUILD_DIR
    ? resolve(REPO_ROOT, process.env.CTSC_BUILD_DIR)
    : BUILD_DIR;
}

/** Executable candidates searched by findCtscExe().
 *  When CTSC_BUILD_DIR is set, only that dir's exe is searched — otherwise
 *  a parallel scanner loop could pick up the parser loop's half-built
 *  binary. */
export function getCtscExeCandidates(): string[] {
  if (process.env.CTSC_BUILD_DIR) {
    const d = resolve(REPO_ROOT, process.env.CTSC_BUILD_DIR);
    return [resolve(d, "ctsc.exe"), resolve(d, "ctsc")];
  }
  return CTSC_EXE_CANDIDATES;
}

/** Default per-phase build dir the loop will use when --phase is set
 *  but CTSC_BUILD_DIR is not forced by the caller. */
export function defaultBuildDirForPhase(phase: string): string {
  return resolve(CTSC_DIR, "build", `phase-${phase}`);
}

/** Default per-phase progress file the loop will use when --phase is set
 *  but CTSC_PROGRESS_FILE is not forced by the caller. */
export function defaultProgressFileForPhase(phase: string): string {
  return resolve(STATE_DIR, `progress.${phase}.json`);
}
