// @checker: types
declare module "lib" {
  export const x: number;
  export const y: string;
}
export { x, y as yy } from "lib";
import { x, yy } from "lib";
const a = x;
const b = yy;
