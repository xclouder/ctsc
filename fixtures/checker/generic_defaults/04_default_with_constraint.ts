// @checker: types
function first<T extends string = "x">(x: T): T {
  return x;
}
const a = first("hello");
const b = first<"y">("y");
