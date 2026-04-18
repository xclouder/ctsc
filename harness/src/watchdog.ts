import { spawnSync } from "node:child_process";
import { appendFile, mkdir } from "node:fs/promises";
import { dirname } from "node:path";

import { DEFERRED_FILE, REPO_ROOT } from "./paths.js";
import type { FixtureStatus } from "./types.js";

export interface WatchdogConfig {
  maxNoProgress: number;
  enableRollback: boolean;
}

export const DEFAULT_WATCHDOG: WatchdogConfig = {
  maxNoProgress: 5,
  enableRollback: false,
};

export async function deferFixture(s: FixtureStatus, reason: string): Promise<void> {
  s.deferred = true;
  await mkdir(dirname(DEFERRED_FILE), { recursive: true });
  await appendFile(DEFERRED_FILE, JSON.stringify({ id: s.id, reason, at: new Date().toISOString() }) + "\n");
}

export function rollbackToTag(tag: string): boolean {
  const r = spawnSync("git", ["-C", REPO_ROOT, "reset", "--hard", tag], { encoding: "utf8" });
  return r.status === 0;
}

export function snapshotTag(tag: string, message = ""): boolean {
  // Use the PowerShell script so git repo is created on demand
  const r = spawnSync("powershell", ["-NoProfile", "-File", "scripts/snapshot.ps1", "-Tag", tag, "-Message", message], {
    cwd: REPO_ROOT,
    encoding: "utf8",
  });
  return r.status === 0;
}
