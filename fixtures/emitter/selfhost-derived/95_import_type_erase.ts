// `import type` and `import { type X }` MUST be erased entirely
// (they exist only for type checking). Mixed imports keep the value
// part and erase the type part.
//
// Expected emit (target=ES2020):
//   - line 1 entirely removed
//   - line 2: `import { value } from "./mod.js";`  (type Foo dropped)
//   - line 3 entirely removed (no .js side-effect remains either)

import type { ConfigShape } from "./types.js";
import { value, type Helper } from "./mod.js";
import { type OnlyTypes } from "./other.js";

export function get(): number {
  return value();
}

export type Re = ConfigShape | Helper | OnlyTypes;
