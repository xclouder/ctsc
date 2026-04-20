import { count, describe } from "./dist/index.js";
import { readdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const distDir = join(here, "dist");

const n = count("  one two   three  four ");
if (n !== 4) throw new Error(`p05 count expected 4, got ${n}`);

const d = describe("a b c");
if (d !== "n=3 values=a,b,c") throw new Error(`p05 describe got ${d}`);

// Confirm files[] was honored: notInFiles.ts must NOT be compiled.
async function walk(dir) {
  const out = [];
  for (const e of await readdir(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...(await walk(p)));
    else out.push(p);
  }
  return out;
}
const compiled = await walk(distDir);
const leaked = compiled.filter((p) => /notInFiles/.test(p));
if (leaked.length > 0) throw new Error(`p05 files[] leaked: ${leaked.join(", ")}`);

console.log("p05_files_list: ok");
