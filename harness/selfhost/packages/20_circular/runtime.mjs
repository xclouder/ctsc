import { chain, callA } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(chain(), "AB");
assert.equal(callA(), "A");

console.log("20_circular: ok");
