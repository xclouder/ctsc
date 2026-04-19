import { fromA } from "./a.js";

export function fromB(): string {
  return "B";
}

export function callA(): string {
  return fromA();
}
