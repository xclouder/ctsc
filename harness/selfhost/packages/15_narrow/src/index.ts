export function formatPrimitive(v: unknown): string {
  if (typeof v === "string") return "s:" + v;
  if (typeof v === "number") return "n:" + v;
  if (typeof v === "boolean") return v ? "t" : "f";
  if (v === null) return "null";
  if (v === undefined) return "undef";
  return "other";
}

export class HttpError extends Error {
  constructor(public status: number, message: string) {
    super(message);
  }
}

export function describeError(e: unknown): string {
  if (e instanceof HttpError) return "http:" + e.status;
  if (e instanceof Error) return "err:" + e.message;
  if (typeof e === "string") return "raw:" + e;
  return "unknown";
}

export type Shape =
  | { kind: "circle"; r: number }
  | { kind: "square"; side: number }
  | { kind: "rect"; w: number; h: number };

export function area(s: Shape): number {
  switch (s.kind) {
    case "circle": return Math.PI * s.r * s.r;
    case "square": return s.side * s.side;
    case "rect":   return s.w * s.h;
  }
}
