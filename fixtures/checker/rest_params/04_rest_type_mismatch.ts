// @checker: diag
function sum(...nums: number[]): number {
  let s = 0;
  for (const n of nums) s += n;
  return s;
}
const r = sum(1, "two", 3);
