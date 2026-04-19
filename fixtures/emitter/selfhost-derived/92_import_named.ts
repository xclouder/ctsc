// M2 baseline bug B18: named imports are not parsed.
// ctsc emits the destructuring as a block+expression and strands `from "..."`.
// Expected (tsc transpileModule, ES2020 = preserveModules):
// the import declaration is preserved verbatim.

import { add, mul } from "./math.js";
import { bracket, pad as padLeft } from "./format.js";

export function summary(values: number[]): string {
  let total = 0;
  let product = 1;
  for (const v of values) {
    total = add(total, v);
    product = mul(product, v);
  }
  return bracket(padLeft(String(total), 4) + "/" + padLeft(String(product), 4));
}
