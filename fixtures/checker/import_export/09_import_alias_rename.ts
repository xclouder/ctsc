// @checker: types
declare module "lib" {
  export const longName: number;
  export function f(n: number): boolean;
}
import { longName as ln, f as fn } from "lib";
const a = ln;
const b = fn(1);
