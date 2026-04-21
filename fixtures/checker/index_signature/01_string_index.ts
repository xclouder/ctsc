// @checker: types
interface Dict {
  [key: string]: number;
}
const d: Dict = { a: 1, b: 2 };
const n = d["a"];
