import { add, mul } from "./math.js";
import { bracket, pad } from "./format.js";

export function summary(values: number[]): string {
  let total = 0;
  let product = 1;
  for (const v of values) {
    total = add(total, v);
    product = mul(product, v);
  }
  return bracket(pad(String(total), 4) + "/" + pad(String(product), 4));
}
