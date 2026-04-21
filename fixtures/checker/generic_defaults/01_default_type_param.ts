// @checker: types
function id<T = number>(x: T): T {
  return x;
}
const a = id(1);
const b = id("x");
