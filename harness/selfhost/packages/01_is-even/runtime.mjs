import { isEven, isOdd } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(isEven(0), true);
assert.equal(isEven(2), true);
assert.equal(isEven(3), false);
assert.equal(isEven(NaN), false);
assert.equal(isOdd(1), true);
assert.equal(isOdd(4), false);
console.log("01_is-even OK");
