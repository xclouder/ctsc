export function total(xs: number[]): number {
  let sum = 0;
  for (const x of xs) {
    sum += x;
  }
  return sum;
}

export function pluckNames(users: { name: string }[]): string[] {
  const out: string[] = [];
  for (const u of users) {
    out.push(u.name);
  }
  return out;
}
