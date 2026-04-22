// @checker: types
type Obj = { a: number; b: string; c: boolean };
type K = keyof Obj;
declare const k: K;
const x = k;
