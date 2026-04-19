export function sum(xs: number[]): number {
  let s = 0;
  for (const x of xs) s += x;
  return s;
}

export function max(xs: number[]): number {
  let m = -Infinity;
  for (const x of xs) if (x > m) m = x;
  return m;
}
