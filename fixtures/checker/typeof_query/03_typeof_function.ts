// @checker: types
function inc(n: number): number {
  return n + 1;
}
type Inc = typeof inc;
declare const f: Inc;
const r = f(1);
