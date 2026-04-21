// @checker: diag
class Store {
  get(key: "name"): string;
  get(key: "age"): number;
  get(key: string): any {
    return key;
  }
}
declare const s: Store;
const r = s.get(123);
