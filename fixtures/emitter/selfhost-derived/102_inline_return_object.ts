// B19.d: `return { a: x, b: y };` with short entries should stay on one
// line. This is the same rule as 100 but in a return-statement context,
// and it's the exact shape that 19_nested/src/core/stats.ts hits.

export function pair(a: number, b: number): { first: number; second: number } {
  return { first: a, second: b };
}

export function analyze(values: number[]): { total: number; peak: number } {
  let total = 0;
  let peak = -Infinity;
  for (const v of values) {
    total += v;
    if (v > peak) peak = v;
  }
  return { total: total, peak: peak };
}
