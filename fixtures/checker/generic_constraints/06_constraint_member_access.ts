// @checker: types
interface Named {
  name: string;
}
function getName<T extends Named>(x: T): string {
  return x.name;
}
const r = getName({ name: "alice", age: 30 });
