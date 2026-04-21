// @checker: types
class A {
  m(): number {
    return 1;
  }
}
class B extends A {}
const n = new B().m();
