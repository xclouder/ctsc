// @checker: types
const obj = { a: 1, b: "x" };
type O = typeof obj;
declare const p: O;
const a = p.a;
const b = p.b;
