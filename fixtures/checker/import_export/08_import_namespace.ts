// @checker: types
declare module "math" {
  export const PI: number;
  export function abs(x: number): number;
}
import * as M from "math";
const a = M.PI;
const b = M.abs(-1);
