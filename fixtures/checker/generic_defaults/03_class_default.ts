// @checker: types
class Box<T = number> {
  value: T;
  constructor(v: T) {
    this.value = v;
  }
}
declare const a: Box;
declare const b: Box<string>;
const x = a.value;
const y = b.value;
