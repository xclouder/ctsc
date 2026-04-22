// @checker: types
type IsString<T> = T extends string ? true : false;
type A = IsString<"hello">;
type B = IsString<number>;
declare const a: A;
declare const b: B;
const x = a;
const y = b;
