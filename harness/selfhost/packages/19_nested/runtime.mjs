import { analyze, sum, max } from "./dist/index.js";
import assert from "node:assert/strict";

const s = analyze([3, 1, 4, 1, 5, 9, 2, 6], ["a", "b", "c"]);
assert.equal(s.total, 31);
assert.equal(s.peak, 9);
assert.equal(s.label, "a,b,c");

assert.equal(sum([1, 2, 3]), 6);
assert.equal(max([-1, -5, -2]), -1);

console.log("19_nested: ok");
