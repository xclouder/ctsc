export function pad(s: string, width: number, ch = " "): string {
  if (s.length >= width) return s;
  return ch.repeat(width - s.length) + s;
}

export function bracket(s: string): string {
  return "[" + s + "]";
}
