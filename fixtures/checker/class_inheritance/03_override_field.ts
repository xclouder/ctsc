// @checker: types
class A {
  x: number = 1;
}
class B extends A {
  x: number = 2;
}
const b = new B();
const n = b.x;
