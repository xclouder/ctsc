// @checker: types
class A {
  m(): string {
    return "a";
  }
}
class B extends A {
  m(): string {
    return "b";
  }
}
const s = new B().m();
