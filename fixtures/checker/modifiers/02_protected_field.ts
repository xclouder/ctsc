// @checker: types
class A {
  protected x: number = 1;
}
class B extends A {
  get(): number {
    return this.x;
  }
}
const n = new B().get();
