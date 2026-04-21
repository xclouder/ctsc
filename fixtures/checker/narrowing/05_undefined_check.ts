// @checker: types
function f(x: number | undefined) {
  if (x !== undefined) {
    const n = x;
  }
}
