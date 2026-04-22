// @checker: types
function keys<T>(o: T): (keyof T)[] {
  return [] as any;
}
const ks = keys({ a: 1, b: 2 });
