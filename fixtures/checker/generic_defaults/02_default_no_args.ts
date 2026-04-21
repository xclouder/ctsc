// @checker: types
function make<T = string>(): T | undefined {
  return undefined;
}
const a = make();
const b = make<number>();
