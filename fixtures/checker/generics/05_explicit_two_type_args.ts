// @checker: types
function first<A, B>(a: A, b: B): B {
  return b;
}
const s = first<number, string>(1, "x");
