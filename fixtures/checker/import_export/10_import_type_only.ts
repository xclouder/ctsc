// @checker: types
declare module "types" {
  export interface User { id: number; name: string; }
  export type Id = number;
}
import type { User, Id } from "types";
declare const u: User;
declare const i: Id;
const a = u;
const b = i;
