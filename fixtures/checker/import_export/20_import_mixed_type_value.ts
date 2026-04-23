// @checker: types
declare module "mix-types" {
  export interface User {
    id: number;
  }
  export const version: number;
}
import { type User, version } from "mix-types";
declare const u: User;
const a = u;
const b = version;
