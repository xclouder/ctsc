// @checker: types
function h(x: "a"): number;
function h(x: string): string;
function h(x: string): any {
  return x === "a" ? 1 : x;
}
const a = h("a");
const b = h("other");
