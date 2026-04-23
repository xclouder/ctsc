// @checker: types
declare module "lib2" {
  export const n: number;
  export function id(x: number): number;
}
export * from "lib2";
import { n, id } from "lib2";
const a = n;
const b = id(1);
