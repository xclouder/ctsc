// @checker: types
declare module "mix" {
  export default function fn(n: number): number;
  export const tag: string;
}
import fn, { tag } from "mix";
const a = fn(1);
const b = tag;
