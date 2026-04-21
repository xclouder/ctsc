// @checker: types
function f(b: boolean) {
  if (b) {
    return 1;
  }
  return "a";
}
const v = f(true);
