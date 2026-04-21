// @checker: types
function f(b: boolean) {
  if (b) {
    return 1;
  }
  return 2;
}
const n = f(true);
