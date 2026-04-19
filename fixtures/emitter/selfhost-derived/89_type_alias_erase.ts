export type ID = string;
export type Predicate<T> = (x: T) => boolean;

export type Shape =
  | { kind: "circle"; r: number }
  | { kind: "square"; side: number };

type InternalHelper = number | string;

export function pick<T>(xs: T[], pred: Predicate<T>): T[] {
  const out: T[] = [];
  for (const x of xs) {
    if (pred(x)) out.push(x);
  }
  return out;
}

export function describe(s: Shape): string {
  return s.kind === "circle" ? "c:" + s.r : "s:" + s.side;
}
