// @checker: types
declare module "pack" {
  export const a: number;
  export function f(x: number): number;
}
export * as P from "pack";
import { a, f } from "pack";
const x = a;
const y = f(1);
