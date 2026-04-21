// @checker: types
class Box {
  value: number = 0;
}
type BoxCtor = typeof Box;
declare const Ctor: BoxCtor;
const b = new Ctor();
