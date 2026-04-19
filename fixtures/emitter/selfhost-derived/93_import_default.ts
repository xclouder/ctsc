// Default import + side-effect import + namespace import.
// Each shape must be preserved verbatim under target=ES2020.

import fs from "node:fs";
import * as path from "node:path";
import "./polyfill.js";

export function readManifest(dir: string): string {
  return fs.readFileSync(path.join(dir, "package.json"), "utf8");
}
