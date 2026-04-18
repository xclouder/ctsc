import { greet, block, stamped } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(greet("ada", 7), "hello, ada! you are #7");
assert.equal(block("T", "body"), "== T ==\nbody\n== end ==");
assert.equal(stamped, "[<INFO>] x=<42>");
console.log("08_template-lit OK");
