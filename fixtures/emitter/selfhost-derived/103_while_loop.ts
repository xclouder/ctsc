// Bug B21: `while` statement body is elided entirely.
// M3 first project probe exposed this: the gcd implementation's entire
// while block disappeared, leaving `a = Math.abs(a); b = Math.abs(b); return a;`
// and silently giving wrong results at runtime.
//
// No previous emitter fixture contained a `while` statement -- the only
// loops tested were `for`/`for-of`, which use a different parser path.

export function gcd(a: number, b: number): number {
  a = Math.abs(a);
  b = Math.abs(b);
  while (b !== 0) {
    const t = b;
    b = a % b;
    a = t;
  }
  return a;
}

export function countdown(n: number): number {
  let steps = 0;
  while (n > 0) {
    n--;
    steps++;
  }
  return steps;
}

export function firstTrue<T>(xs: T[], p: (x: T) => boolean): T | undefined {
  let i = 0;
  while (i < xs.length) {
    if (p(xs[i])) return xs[i];
    i++;
  }
  return undefined;
}
