// @checker: types
function first<A, B>(a: A, b: B): A {
  return a;
}
const n = first(1, "x");
