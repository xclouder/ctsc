import { gcd, lcm } from "./math.js";

export function summarize(a: number, b: number): { gcd: number; lcm: number } {
  return { gcd: gcd(a, b), lcm: lcm(a, b) };
}
