import { greet, times } from "./lib.js";

export function banner(name: string, n: number): string {
  return times(greet(name), n);
}
