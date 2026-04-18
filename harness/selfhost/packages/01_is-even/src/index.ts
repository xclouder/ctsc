export function isEven(n: number): boolean {
  return typeof n === "number" && !Number.isNaN(n) && n % 2 === 0;
}

export function isOdd(n: number): boolean {
  return !isEven(n);
}
