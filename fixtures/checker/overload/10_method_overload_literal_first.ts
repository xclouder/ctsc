// @checker: types
class Fetcher {
  fetch(url: "users"): { id: number; name: string };
  fetch(url: string): any;
  fetch(url: string): any {
    return url === "users" ? { id: 1, name: "a" } : null;
  }
}
declare const f: Fetcher;
const u = f.fetch("users");
const o = f.fetch("other");
