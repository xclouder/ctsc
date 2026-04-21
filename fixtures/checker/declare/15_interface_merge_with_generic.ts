// @checker: types
interface Box<T> {
  value: T;
}
interface Box<T> {
  kind: string;
}
declare const b: Box<number>;
const v = b.value;
const k = b.kind;
