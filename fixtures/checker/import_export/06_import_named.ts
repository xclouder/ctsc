// @checker: types
declare module "lib" {
  export const x: number;
  export function f(n: number): string;
}
import { x, f } from "lib";
const a = x;
const b = f(1);
