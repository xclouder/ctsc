// @checker: types
class A {
  greet(): string {
    return "a";
  }
}
class B extends A {
  greet(): string {
    return super.greet();
  }
}
const s = new B().greet();
