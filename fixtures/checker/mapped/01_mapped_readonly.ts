// @checker: types
interface Point { x: number; y: number; }
type ReadonlyPoint = { readonly [K in keyof Point]: Point[K] };
declare const p: ReadonlyPoint;
const a = p;
