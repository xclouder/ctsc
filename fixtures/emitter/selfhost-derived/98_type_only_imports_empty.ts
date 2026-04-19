// Companion to 97: a file that imports types only and has value-level
// declarations. `import type`, `import { type X }` and `export type`
// must all erase while the rest emits verbatim. The file stays a
// module because it still has a value-level export.

import type { TickEvent } from "./types.js";
import { type ErrorEvent, makeBus } from "./helpers.js";

export type Handler = (e: TickEvent) => void;

export function wireTicks(h: Handler): void {
  makeBus().on(h as (e: unknown) => void);
}
