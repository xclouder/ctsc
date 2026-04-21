// @checker: types
class C {
  add(x: number, y: number): number {
    return x + y;
  }
}
const n = new C().add(1, 2);
