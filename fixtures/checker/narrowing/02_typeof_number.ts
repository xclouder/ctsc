// @checker: types
function f(x: string | number) {
  if (typeof x === "number") {
    const n = x;
  }
}
