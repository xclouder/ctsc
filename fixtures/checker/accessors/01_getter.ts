// @checker: types
class C {
  _x: number = 1;
  get x(): number {
    return this._x;
  }
}
const n = new C().x;
