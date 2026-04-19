import { clamp, lerp, repeat, reverse } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(clamp(5, 0, 10), 5);
assert.equal(clamp(-3, 0, 10), 0);
assert.equal(clamp(99, 0, 10), 10);
assert.equal(lerp(0, 10, 0.5), 5);
assert.equal(lerp(10, 20, 0), 10);
assert.equal(repeat("ab", 3), "ababab");
assert.equal(repeat("x", 0), "");
assert.equal(reverse("hello"), "olleh");
assert.equal(reverse(""), "");

console.log("17_barrel: ok");
