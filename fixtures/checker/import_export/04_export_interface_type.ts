// @checker: types
export interface User {
  id: number;
  name: string;
}
export type Id = number;
declare const u: User;
declare const i: Id;
const a = u;
const b = i;
