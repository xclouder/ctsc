// @checker: types
class User {
  name: string = "";
  age: number = 0;
}
type K = keyof User;
declare const k: K;
const x = k;
