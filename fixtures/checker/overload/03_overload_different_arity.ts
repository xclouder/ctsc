// @checker: types
function g(x: number): string;
function g(x: number, y: number): number;
function g(x: number, y?: number): any {
  return y === undefined ? "one" : x + y;
}
const a = g(1);
const b = g(1, 2);
