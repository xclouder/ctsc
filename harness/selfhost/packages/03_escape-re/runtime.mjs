import { escapeStringRegexp } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(escapeStringRegexp("a.b"), "a\\.b");
assert.equal(escapeStringRegexp("(x)"), "\\(x\\)");
assert.equal(escapeStringRegexp("a-b"), "a\\x2db");
assert.throws(() => escapeStringRegexp(42));
console.log("03_escape-re OK");
