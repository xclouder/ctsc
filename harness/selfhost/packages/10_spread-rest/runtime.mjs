import { concat, withDefaults, headTail } from "./dist/index.js";
import assert from "node:assert/strict";

assert.deepEqual(concat([1, 2], [3], [4, 5]), [1, 2, 3, 4, 5]);
assert.deepEqual(concat(), []);

assert.deepEqual(
  withDefaults({ port: 8080 }),
  { host: "localhost", port: 8080, tls: false },
);

const [h, t] = headTail([10, 20, 30]);
assert.equal(h, 10);
assert.deepEqual(t, [20, 30]);

console.log("10_spread-rest OK");
