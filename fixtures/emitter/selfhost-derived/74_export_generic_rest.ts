export function concat<T>(...arrs: T[][]): T[] {
  const out: T[] = [];
  for (const a of arrs) {
    out.push(...a);
  }
  return out;
}

export function debounce<T extends (...args: any[]) => void>(
  fn: T,
  wait: number,
): (...args: Parameters<T>) => void {
  let timer: ReturnType<typeof setTimeout> | null = null;
  return function (this: unknown, ...args: Parameters<T>): void {
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => fn.apply(this, args), wait);
  };
}
