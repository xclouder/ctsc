// @checker: types
class Store {
  get(key: "name"): string;
  get(key: "age"): number;
  get(key: string): any {
    return key === "name" ? "alice" : 42;
  }
}
declare const s: Store;
const n = s.get("name");
const a = s.get("age");
