// @checker: types
class A {
  x: number = 1;
}
class B extends A {}
const b = new B();
const n = b.x;
