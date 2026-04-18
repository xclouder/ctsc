import { ms } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(ms("1s"), 1000);
assert.equal(ms("30s"), 30_000);
assert.equal(ms("5m"), 5 * 60_000);
assert.equal(ms("2h"), 2 * 3_600_000);
assert.equal(ms("1d"), 86_400_000);
assert.throws(() => ms("abc"));
console.log("02_ms-lite OK");
