// @checker: types
class Box<T> {
  value: T;
  constructor(v: T) {
    this.value = v;
  }
}
const b = new Box(1);
const n = b.value;
