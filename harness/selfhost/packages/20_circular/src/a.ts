import { fromB } from "./b.js";

export function fromA(): string {
  return "A";
}

export function chain(): string {
  return fromA() + fromB();
}
