// @checker: types
interface User {
  id: number;
  name: string;
}
type Id = number;
export type { User, Id };
declare const u: User;
declare const i: Id;
const a = u;
const b = i;
