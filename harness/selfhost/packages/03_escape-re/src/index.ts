export function escapeStringRegexp(s: string): string {
  if (typeof s !== "string") {
    throw new TypeError("Expected a string");
  }
  return s.replace(/[|\\{}()[\]^$+*?.]/g, "\\$&").replace(/-/g, "\\x2d");
}
