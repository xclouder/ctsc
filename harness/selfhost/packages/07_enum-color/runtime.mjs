import { Color, names, nameOf } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(Color.Red, 0);
assert.equal(Color.Green, 1);
assert.equal(Color.Blue, 2);
assert.equal(names[Color.Red], "red");
assert.equal(nameOf(Color.Green), "green");
console.log("07_enum-color OK");
