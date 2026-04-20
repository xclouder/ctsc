// Control-flow keywords that are rarely exercised by fixtures so far:
// bare `break`, `continue`, and labeled break/continue. All should be
// emitted verbatim under target=ES2020.

export function firstPairSumming(xs: number[], target: number): [number, number] | null {
  outer: for (let i = 0; i < xs.length; i++) {
    for (let j = i + 1; j < xs.length; j++) {
      if (xs[i] + xs[j] === target) {
        return [xs[i], xs[j]];
      }
      if (xs[j] > target) {
        continue outer;
      }
    }
  }
  return null;
}

export function trim<T>(xs: T[], keep: (x: T) => boolean): T[] {
  const out: T[] = [];
  for (const x of xs) {
    if (!keep(x)) break;
    out.push(x);
  }
  return out;
}

export function skipOdds(xs: number[]): number[] {
  const out: number[] = [];
  for (const x of xs) {
    if (x % 2 !== 0) continue;
    out.push(x);
  }
  return out;
}
