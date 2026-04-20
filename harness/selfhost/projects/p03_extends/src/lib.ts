export function greet(name: string): string {
  return `hello, ${name}`;
}

export function times(s: string, n: number): string {
  const parts: string[] = [];
  for (let i = 0; i < n; i++) {
    parts.push(s);
  }
  return parts.join(" ");
}
