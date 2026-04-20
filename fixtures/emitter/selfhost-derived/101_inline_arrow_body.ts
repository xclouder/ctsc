// B19.c: arrow function with a block body containing exactly one
// short statement should print as `(x) => { stmt; }` on one line,
// not split the braces.

export const incAll = (xs: number[]): number[] => xs.map((x) => { return x + 1; });

export function register(on: (fn: (e: number) => void) => void): void {
  on((e) => { console.log(e); });
  on((e) => { if (e > 0) console.log("+"); });
}

export const pick = <T>(arr: T[], i: number): T | undefined => {
  return arr[i];
};
