// @checker: types
function arr<T>(...items: T[]): T[] {
  return items;
}
const a = arr(1, 2, 3);
const b = arr("x", "y");
