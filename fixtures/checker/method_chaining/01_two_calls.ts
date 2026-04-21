// @checker: types
class C {
  self(): C {
    return this;
  }
  value(): number {
    return 1;
  }
}
const n = new C().self().value();
