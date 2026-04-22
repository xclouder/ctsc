// @checker: types
function pick<T, K extends keyof T>(o: T, k: K): T[K] {
  return o[k];
}
const o = { a: 1, b: "x" };
const a = pick(o, "a");
const b = pick(o, "b");
