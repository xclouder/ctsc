// @checker: types
interface Big { a: number; b: string; c: boolean; }
type Pick2<T, K extends keyof T> = { [P in K]: T[P] };
type R = Pick2<Big, "a" | "b">;
declare const r: R;
const x = r;
