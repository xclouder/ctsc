import { summarize } from "./dist/index.js";
import assert from "node:assert/strict";

assert.deepEqual(summarize(12, 18), { gcd: 6, lcm: 36 });
assert.deepEqual(summarize(7, 13),  { gcd: 1, lcm: 91 });
assert.deepEqual(summarize(0, 5),   { gcd: 5, lcm: 0 });

console.log("p01_simple: ok");
