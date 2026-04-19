export function repeat(s: string, n: number): string {
  let out = "";
  for (let i = 0; i < n; i++) out += s;
  return out;
}

export function reverse(s: string): string {
  let out = "";
  for (let i = s.length - 1; i >= 0; i--) out += s[i];
  return out;
}
