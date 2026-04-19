export class AppError extends Error {
  constructor(public readonly code: string, message: string) {
    super(message);
    this.name = "AppError";
  }
}

export function parseIntOrThrow(s: string): number {
  const n = Number(s);
  if (Number.isNaN(n)) {
    throw new AppError("E_PARSE", "not a number: " + s);
  }
  return n;
}

export function safeParse(s: string): { ok: true; value: number } | { ok: false; code: string } {
  try {
    return { ok: true, value: parseIntOrThrow(s) };
  } catch (err: unknown) {
    if (err instanceof AppError) {
      return { ok: false, code: err.code };
    }
    return { ok: false, code: "E_UNKNOWN" };
  }
}

export function runWithCleanup<T>(fn: () => T, cleanup: () => void): T {
  try {
    return fn();
  } finally {
    cleanup();
  }
}
