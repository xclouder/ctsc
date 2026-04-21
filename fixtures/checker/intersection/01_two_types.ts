// @checker: types
interface A {
  x: number;
}
interface B {
  y: string;
}
const ab: A & B = { x: 1, y: "hi" };
const n = ab.x;
const s = ab.y;
