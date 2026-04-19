// Re-export shapes (export-from). All four forms must be preserved
// (only `export type` is erased).

export { add, mul } from "./math.js";
export { default as Engine } from "./engine.js";
export * from "./util.js";
export * as fmt from "./format.js";
export type { Shape } from "./shape.js";
