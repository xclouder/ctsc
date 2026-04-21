// @checker: types
class A {
  x: number = 1;
  get(): number {
    return this.x;
  }
}
class B extends A {
  get(): number {
    return super.get() + 1;
  }
}
const n = new B().get();
