// @checker: types
function id<T>(x: T): T {
  return x;
}
const n = id<number>(42);
