// @checker: types
function len<T extends { length: number }>(x: T): number {
  return x.length;
}
const a = len("hello");
const b = len([1, 2, 3]);
