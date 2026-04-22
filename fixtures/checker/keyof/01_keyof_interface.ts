// @checker: types
interface Point {
  x: number;
  y: number;
}
type K = keyof Point;
declare const k: K;
const a = k;
