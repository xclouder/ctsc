// @checker: types
export function add(a: number, b: number): number { return a + b; }
export class Point {
  x: number = 0;
  y: number = 0;
}
const r = add(1, 2);
const p = new Point();
