import { banner } from "./dist/index.js";

const out = banner("ada", 3);
const expected = "hello, ada hello, ada hello, ada";
if (out !== expected) {
  throw new Error(`p03 banner expected ${JSON.stringify(expected)}, got ${JSON.stringify(out)}`);
}
console.log("p03_extends: ok");
