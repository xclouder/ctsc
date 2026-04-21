// @checker: types
function id<T>(x: T): T {
  return x;
}
const n = id(42);
