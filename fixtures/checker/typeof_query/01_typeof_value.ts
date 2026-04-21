// @checker: types
const x = 42;
type X = typeof x;
declare const y: X;
const z = y;
