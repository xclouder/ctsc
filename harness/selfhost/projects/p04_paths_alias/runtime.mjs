import { summary } from "./dist/index.js";

const out = summary(3);
const expected = "core@1.0.0: sq=9 cu=27";
if (out !== expected) {
  throw new Error(`p04 summary expected ${JSON.stringify(expected)}, got ${JSON.stringify(out)}`);
}
console.log("p04_paths_alias: ok");
