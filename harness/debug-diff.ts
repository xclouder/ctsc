import { readFile } from "node:fs/promises";
import { runCtsc, findCtscExe } from "./src/runner.js";
import { loadCurriculum as listFixtures } from "./src/curriculum.js";
import { getOracle } from "./src/oracle.js";
import { diffPhase } from "./src/differ.js";

const id = "parser/from-upstream/107_parserModuleDeclaration1.ts";
const fx = (await listFixtures()).find(f => f.id === id)!;
const src = await readFile(fx.sourcePath, "utf8");
const oracle = await getOracle(fx, src);
const expected = oracle.tokensJson ?? oracle.astJson ?? oracle.emitJs ?? "";
const exe = await findCtscExe();
console.log("exe:", exe);
const run = await runCtsc(fx, {});
console.log("=== EXPECTED ===");
console.log(expected);
console.log("=== ACTUAL ===");
console.log(run.stdout);
console.log("=== DIFF ===");
const d = diffPhase(fx.phase, expected, run.stdout);
console.log(d.summary);
if (!d.equal && d.firstMismatch) {
  console.log("  expected:", d.firstMismatch.expected);
  console.log("  actual:  ", d.firstMismatch.actual);
}
