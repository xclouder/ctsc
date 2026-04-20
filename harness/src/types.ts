export type Phase = "scanner" | "parser" | "binder" | "checker" | "emitter" | "cli";

/** Which channel a checker-phase fixture aligns on.
 *  - "types": compare `--dump-types` output vs ts.TypeChecker.typeToString dump
 *  - "diag":  compare `--check` output vs ts.Program.getSemanticDiagnostics
 *  - "both":  reserved for M4.1+; treated as "types" for now with a followup. */
export type CheckerChannel = "types" | "diag" | "both";

export interface Fixture {
  id: string;
  phase: Phase;
  stage: string;
  sourcePath: string;
  /** Relative path used for oracle cache key & report names. */
  relPath: string;
  /** Optional difficulty score, higher is harder. */
  difficulty: number;
  /** Populated only when phase==="checker"; resolved from fixture comment. */
  checkerChannel?: CheckerChannel;
}

export interface OracleArtifacts {
  tokensJson?: string;
  astJson?: string;
  emitJs?: string;
  emitDts?: string;
  diagnostics?: string;
  /** Checker channel payloads; exactly one is populated for checker fixtures. */
  checkerDiagJson?: string;
  checkerTypesJson?: string;
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
