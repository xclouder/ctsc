import { cityOf, zipOf, firstLen } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(cityOf(null), "unknown");
assert.equal(cityOf({ name: "x" }), "unknown");
assert.equal(cityOf({ name: "x", addr: {} }), "unknown");
assert.equal(cityOf({ name: "x", addr: { city: "NYC" } }), "NYC");

assert.equal(zipOf(undefined), -1);
assert.equal(zipOf({ name: "x", addr: { zip: 90210 } }), 90210);

assert.equal(firstLen(undefined), 0);
assert.equal(firstLen([]), 0);
assert.equal(firstLen(["hi", "there"]), 2);

console.log("09_optional-chain OK");
