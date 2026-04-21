// @checker: types
function first<T extends string>(x: T): T {
  return x;
}
const a = first("a");
const b = first("hello");
