import { debounce } from "./dist/index.js";
import assert from "node:assert/strict";

let n = 0;
const d = debounce((v) => { n += v; }, 10);
d(1); d(2); d(3);
await new Promise((r) => setTimeout(r, 30));
assert.equal(n, 3, "only the last call should fire");
console.log("05_debounce OK");
