// @checker: types
declare class Greeter {
  greet(name: string): string;
}
declare const g: Greeter;
const s = g.greet("world");
