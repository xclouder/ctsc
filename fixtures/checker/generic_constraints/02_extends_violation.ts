// @checker: diag
function len<T extends { length: number }>(x: T): number {
  return x.length;
}
const r = len(42);
