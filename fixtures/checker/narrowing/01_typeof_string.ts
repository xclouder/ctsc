// @checker: types
function f(x: string | number) {
  if (typeof x === "string") {
    const s = x;
  } else {
    const n = x;
  }
}
