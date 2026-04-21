// @checker: types
class C {
  x: number = 1;
  get(): number {
    return this.x;
  }
  doubled(): number {
    return this.get() + this.get();
  }
}
const c = new C();
const n = c.doubled();
