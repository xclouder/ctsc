// @checker: types
function sum(...nums: number[]): number {
  let s = 0;
  for (const n of nums) s += n;
  return s;
}
const xs = [2, 3];
const a = sum(1, ...xs, 4);
