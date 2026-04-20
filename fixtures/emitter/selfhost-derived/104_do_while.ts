// Companion to 103: `do { ... } while (...);` statement.
// Shares the same parser path as `while` in tsc; likely also broken
// in ctsc since 103 shows while-family coverage was zero.

export function rollUntil(target: number, rng: () => number): number {
  let v: number;
  let tries = 0;
  do {
    v = Math.floor(rng() * 100);
    tries++;
  } while (v !== target && tries < 1000);
  return tries;
}

export function drain<T>(arr: T[], take: (x: T) => void): void {
  if (arr.length === 0) return;
  do {
    const x = arr.pop();
    if (x !== undefined) take(x);
  } while (arr.length > 0);
}
