import { summary } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(summary([1, 2, 3]), "[   6/   6]");
assert.equal(summary([2, 5]),    "[   7/  10]");
assert.equal(summary([10]),      "[  10/  10]");
assert.equal(summary([]),        "[   0/   1]");

console.log("16_multifile: ok");
