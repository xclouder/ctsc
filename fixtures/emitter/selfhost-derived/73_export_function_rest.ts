export function sumAll(...nums: number[]): number {
  let total = 0;
  for (const n of nums) {
    total += n;
  }
  return total;
}

export function tag(label: string, ...parts: string[]): string {
  return "[" + label + "] " + parts.join(" ");
}
