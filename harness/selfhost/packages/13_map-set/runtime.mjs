import { Counter, dedupe } from "./dist/index.js";
import assert from "node:assert/strict";

const c = new Counter();
c.bump("a");
c.bump("a");
c.bump("b", 5);
assert.equal(c.get("a"), 2);
assert.equal(c.get("b"), 5);
assert.equal(c.get("missing"), 0);
assert.equal(c.total(), 7);
assert.deepEqual(c.keys().sort(), ["a", "b"]);

assert.deepEqual(dedupe([1, 2, 2, 3, 1, 4]), [1, 2, 3, 4]);
assert.deepEqual(dedupe(["x", "x", "y"]), ["x", "y"]);

console.log("13_map-set OK");
