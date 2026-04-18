function total(xs: number[]): number {
  let sum = 0;
  for (const x of xs) {
    sum += x;
  }
  return sum;
}

function joinNames(users: { name: string }[]): string {
  const names: string[] = [];
  for (const u of users) {
    names.push(u.name);
  }
  return names.join(",");
}
