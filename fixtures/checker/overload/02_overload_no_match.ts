// @checker: diag
function f(x: number): string;
function f(x: string): number;
function f(x: any): any {
  return x;
}
const r = f(true);
