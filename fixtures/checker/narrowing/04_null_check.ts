// @checker: types
function f(x: string | null) {
  if (x !== null) {
    const s = x;
  }
}
