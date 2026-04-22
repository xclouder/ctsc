// @checker: types
interface Obj {
  a: number;
  b: string;
}
type V = Obj[keyof Obj];
declare const v: V;
const x = v;
