// @checker: types
interface User { name: string; age: number; }
type Partial2<T> = { [K in keyof T]?: T[K] };
type PU = Partial2<User>;
declare const u: PU;
const a = u;
