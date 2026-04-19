export class Counter {
  private counts = new Map<string, number>();

  bump(key: string, by: number = 1): void {
    const cur = this.counts.get(key) ?? 0;
    this.counts.set(key, cur + by);
  }

  get(key: string): number {
    return this.counts.get(key) ?? 0;
  }

  keys(): string[] {
    const out: string[] = [];
    for (const k of this.counts.keys()) {
      out.push(k);
    }
    return out;
  }

  total(): number {
    let sum = 0;
    for (const [, v] of this.counts) {
      sum += v;
    }
    return sum;
  }
}

export function dedupe<T>(xs: T[]): T[] {
  const seen = new Set<T>();
  const out: T[] = [];
  for (const x of xs) {
    if (!seen.has(x)) {
      seen.add(x);
      out.push(x);
    }
  }
  return out;
}
