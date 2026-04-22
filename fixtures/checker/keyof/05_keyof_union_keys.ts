// @checker: types
interface Mix {
  id: number;
  name: string;
  active: boolean;
}
type K = keyof Mix;
declare const a: K;
declare const b: K;
const x = a;
const y = b;
