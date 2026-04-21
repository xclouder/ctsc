// @checker: types
class C {
  private x: number = 1;
  get(): number {
    return this.x;
  }
}
const n = new C().get();
