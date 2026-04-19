export const backslash: string = "\\";
export const dollarRef: string = "\\$&";
export const hexEsc: string = "\\x2d";
export const newlineLit: string = "line1\nline2";
export const tabLit: string = "a\tb";
export const quote: string = "she said \"hi\"";

export function escapeRe(s: string): string {
  return s.replace(/[|\\{}()[\]^$+*?.]/g, "\\$&").replace(/-/g, "\\x2d");
}
