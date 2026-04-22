// @checker: types
type NonNull<T> = T extends null | undefined ? never : T;
type R = NonNull<string | null | number>;
declare const r: R;
const x = r;
