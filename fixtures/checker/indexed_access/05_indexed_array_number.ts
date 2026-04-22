// @checker: types
declare const arr: number[];
type E = typeof arr[number];
declare const e: E;
const x = e;
