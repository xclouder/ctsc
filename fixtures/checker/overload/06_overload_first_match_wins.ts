// @checker: types
function k(x: 1): "one";
function k(x: 2): "two";
function k(x: number): string;
function k(x: number): any {
  return x === 1 ? "one" : x === 2 ? "two" : "" + x;
}
const a = k(1);
const b = k(2);
const c = k(3);
