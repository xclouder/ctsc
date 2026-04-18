export type Phase = "scanner" | "parser" | "binder" | "checker" | "emitter" | "cli";

export interface Fixture {
  id: string;
  phase: Phase;
  stage: string;
  sourcePath: string;
  /** Relative path used for oracle cache key & report names. */
  relPath: string;
  /** Optional difficulty score, higher is harder. */
  difficulty: number;
}

export interface OracleArtifacts {
  tokensJson?: string;
  astJson?: string;
  emitJs?: string;
  emitDts?: string;
  diagnostics?: string;
}

export interface RunResult {
  exitCode: number;
  stdout: string;
  stderr: string;
  durationMs: number;
  artifact?: string;
}

export interface DiffResult {
  equal: boolean;
  summary: string;
  unifiedDiff?: string;
  firstMismatch?: {
    path: string; // JSON pointer or byte offset
    expected: string;
    actual: string;
  };
}

export interface FixtureStatus {
  id: string;
  phase: Phase;
  stage: string;
  passed: boolean;
  lastAttempt?: string;
  attempts: number;
  noProgressCount: number;
  lastDiffSummary?: string;
  deferred?: boolean;
}

export interface ProgressState {
  schemaVersion: 1;
  startedAt: string;
  updatedAt: string;
  fixtures: Record<string, FixtureStatus>;
}
