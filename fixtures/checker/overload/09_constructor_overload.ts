// @checker: types
class Point {
  x: number;
  y: number;
  constructor(x: number);
  constructor(x: number, y: number);
  constructor(x: number, y: number = 0) {
    this.x = x;
    this.y = y;
  }
}
const p = new Point(1);
const q = new Point(1, 2);
