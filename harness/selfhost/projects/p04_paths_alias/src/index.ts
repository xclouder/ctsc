import { square, cube } from "./utils/math.js";
import { identify } from "./core/index.js";

export function summary(n: number): string {
  return `${identify()}: sq=${square(n)} cu=${cube(n)}`;
}
