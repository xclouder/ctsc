// @checker: types
interface A {
  x: number;
}
interface B extends A {
  y: string;
}
const b: B = { x: 1, y: "hi" };
const n = b.x;
const s = b.y;
