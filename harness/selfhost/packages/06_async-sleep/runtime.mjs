import { sleep, waitAndReturn, sumAfter } from "./dist/index.js";
import assert from "node:assert/strict";

const t0 = Date.now();
await sleep(20);
assert.ok(Date.now() - t0 >= 15, "sleep should wait ~20ms");

const v = await waitAndReturn("hi", 10);
assert.equal(v, "hi");

const n = await sumAfter(2, 3, 10);
assert.equal(n, 5);

console.log("06_async-sleep OK");
