// @checker: types
declare module "types" {
  export interface User {
    id: number;
    name: string;
  }
}
export type { User } from "types";
import type { User } from "types";
declare const u: User;
const x = u;
