export function parseSafe(s: string): number | null {
  try {
    const n = Number(s);
    if (Number.isNaN(n)) throw new Error("nan");
    return n;
  } catch (err: unknown) {
    if (err instanceof Error) {
      return null;
    }
    return null;
  }
}

export function withResource<T>(acquire: () => T, release: (r: T) => void, use: (r: T) => void): void {
  const r = acquire();
  try {
    use(r);
  } finally {
    release(r);
  }
}
