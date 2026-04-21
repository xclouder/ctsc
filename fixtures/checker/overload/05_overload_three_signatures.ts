// @checker: types
function j(x: number): string;
function j(x: string): number;
function j(x: boolean): boolean;
function j(x: any): any {
  return x;
}
const a = j(1);
const b = j("x");
const c = j(true);
