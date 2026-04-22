// @checker: types
type Extract2<T, U> = T extends U ? T : never;
type R = Extract2<"a" | "b" | "c", "a" | "c">;
declare const r: R;
const x = r;
