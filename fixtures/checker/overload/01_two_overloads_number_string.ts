// @checker: types
function f(x: number): string;
function f(x: string): number;
function f(x: any): any {
  return x;
}
const a = f(1);
const b = f("hi");
