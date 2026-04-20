import { demo } from "./dist/index.js";
import { readdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const distDir = join(here, "dist");

const out = demo();
if (out !== "ADA---:20") {
  throw new Error(`p02 demo expected "ADA---:20", got ${JSON.stringify(out)}`);
}

async function walk(dir) {
  const entries = await readdir(dir, { withFileTypes: true });
  const files = [];
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) files.push(...(await walk(p)));
    else files.push(p);
  }
  return files;
}
const compiled = await walk(distDir);
const forbidden = compiled.filter((p) => /skip\.test|legacy/.test(p));
if (forbidden.length > 0) {
  throw new Error(`p02 exclude leaked: ${forbidden.join(", ")}`);
}

console.log("p02_include_glob: ok");
