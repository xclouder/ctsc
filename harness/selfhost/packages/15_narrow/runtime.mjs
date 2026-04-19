import { formatPrimitive, HttpError, describeError, area } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(formatPrimitive("hi"), "s:hi");
assert.equal(formatPrimitive(3), "n:3");
assert.equal(formatPrimitive(true), "t");
assert.equal(formatPrimitive(false), "f");
assert.equal(formatPrimitive(null), "null");
assert.equal(formatPrimitive(undefined), "undef");
assert.equal(formatPrimitive({}), "other");

assert.equal(describeError(new HttpError(404, "nf")), "http:404");
assert.equal(describeError(new Error("boom")), "err:boom");
assert.equal(describeError("str"), "raw:str");
assert.equal(describeError(42), "unknown");

assert.equal(area({ kind: "square", side: 4 }), 16);
assert.equal(area({ kind: "rect", w: 3, h: 5 }), 15);

console.log("15_narrow OK");
