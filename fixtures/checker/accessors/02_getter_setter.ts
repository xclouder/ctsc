// @checker: types
class C {
  _x: number = 1;
  get x(): number {
    return this._x;
  }
  set x(v: number) {
    this._x = v;
  }
}
const c = new C();
const n = c.x;
