// @checker: types
type ReturnType2<T> = T extends (...args: any[]) => infer R ? R : never;
function fn(): number { return 0; }
type R = ReturnType2<typeof fn>;
declare const r: R;
const x = r;
