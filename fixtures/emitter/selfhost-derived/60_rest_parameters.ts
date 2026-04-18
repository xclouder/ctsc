function sum(...nums: number[]): number {
  let total = 0;
  for (const n of nums) {
    total += n;
  }
  return total;
}

function logTag(tag: string, ...parts: string[]): string {
  return "[" + tag + "] " + parts.join(" ");
}
