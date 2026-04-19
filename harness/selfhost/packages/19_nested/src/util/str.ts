export function join(parts: string[], sep: string): string {
  let out = "";
  for (let i = 0; i < parts.length; i++) {
    if (i > 0) out += sep;
    out += parts[i];
  }
  return out;
}
