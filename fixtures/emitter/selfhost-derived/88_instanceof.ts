class AppError extends Error {
  constructor(public readonly code: string, message: string) {
    super(message);
  }
}

export function classify(e: unknown): string {
  if (e instanceof AppError) return "app:" + e.code;
  if (e instanceof TypeError) return "type:" + e.message;
  if (e instanceof Error) return "err:" + e.message;
  return "unknown";
}

export function isArrayOfNumbers(v: unknown): v is number[] {
  if (!(v instanceof Array)) return false;
  for (const x of v) {
    if (typeof x !== "number") return false;
  }
  return true;
}
