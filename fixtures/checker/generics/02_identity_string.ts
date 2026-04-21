// @checker: types
function id<T>(x: T): T {
  return x;
}
const s = id("hi");
