// @checker: types
type Classify<T> =
  T extends string ? "string"
  : T extends number ? "number"
  : T extends boolean ? "boolean"
  : "other";
type A = Classify<"x">;
type B = Classify<42>;
type C = Classify<true>;
type D = Classify<{}>;
declare const a: A;
declare const b: B;
declare const c: C;
declare const d: D;
const w = a;
const x = b;
const y = c;
const z = d;
