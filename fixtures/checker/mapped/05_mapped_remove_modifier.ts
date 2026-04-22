// @checker: types
interface R { readonly a: number; b?: string; }
type Mutable<T> = { -readonly [K in keyof T]: T[K] };
type Required2<T> = { [K in keyof T]-?: T[K] };
type M = Mutable<R>;
type Q = Required2<R>;
declare const m: M;
declare const q: Q;
const x = m;
const y = q;
