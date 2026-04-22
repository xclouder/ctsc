// @checker: types
interface Mix {
  id: number;
  name: string;
}
type V = Mix["id" | "name"];
declare const v: V;
const a = v;
