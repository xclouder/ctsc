// @checker: types
function greet(prefix: string, name: string): string {
  return prefix + " " + name;
}
const args: [string, string] = ["hello", "alice"];
const s = greet(...args);
