// @checker: types
class C {
  x: number = 1;
  get(): number {
    return this.x;
  }
}
const c = new C();
const n = c.get();
