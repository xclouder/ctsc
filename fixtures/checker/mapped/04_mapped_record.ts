// @checker: types
type Record2<K extends string, V> = { [P in K]: V };
type R = Record2<"a" | "b", number>;
declare const r: R;
const x = r;
