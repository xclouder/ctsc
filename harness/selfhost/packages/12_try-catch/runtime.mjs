import { AppError, parseIntOrThrow, safeParse, runWithCleanup } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(parseIntOrThrow("42"), 42);
assert.throws(() => parseIntOrThrow("oops"), AppError);

const ok = safeParse("7");
assert.equal(ok.ok, true);
assert.equal(ok.ok && ok.value, 7);

const bad = safeParse("nope");
assert.equal(bad.ok, false);
assert.equal(!bad.ok && bad.code, "E_PARSE");

let cleaned = false;
const v = runWithCleanup(() => 99, () => { cleaned = true; });
assert.equal(v, 99);
assert.equal(cleaned, true);

console.log("12_try-catch OK");
