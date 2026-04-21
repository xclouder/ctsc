// @checker: types
interface Point {
  x: number;
}
interface Point {
  y: number;
}
declare const p: Point;
const a = p.x;
const b = p.y;
