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
