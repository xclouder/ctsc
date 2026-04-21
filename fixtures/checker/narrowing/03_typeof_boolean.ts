// @checker: types
function f(x: string | boolean) {
  if (typeof x === "boolean") {
    const b = x;
  }
}
