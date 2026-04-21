// @checker: types
function greet(name: string, ...titles: string[]): string {
  return titles.join(" ") + " " + name;
}
const a = greet("alice");
const b = greet("alice", "Dr", "Prof");
