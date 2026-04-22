// @checker: types
interface Point {
  x: number;
  y: number;
}
type X = Point["x"];
declare const v: X;
const a = v;
