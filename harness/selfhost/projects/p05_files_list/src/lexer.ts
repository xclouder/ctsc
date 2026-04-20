export function tokenize(input: string): string[] {
  return input.trim().split(/\s+/).filter((t) => t.length > 0);
}
