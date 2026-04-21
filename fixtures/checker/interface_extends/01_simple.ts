// @checker: types
interface A {
  x: number;
}
interface B extends A {}
const b: B = { x: 1 };
const n = b.x;
