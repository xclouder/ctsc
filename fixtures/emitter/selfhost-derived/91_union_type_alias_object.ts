// Companion to fixture 90: same union-of-object-types pattern but bound
// to a `type` alias before being used as a return type and a parameter type.
// Helps localize whether the bug is in `parseType` (union continuation) or
// in `parseFunctionReturnType` specifically.

type Result = { ok: true; value: number } | { ok: false; code: string };
type Shape = { kind: "circle"; r: number } | { kind: "square"; side: number };

export function area(s: Shape): number {
  switch (s.kind) {
    case "circle":
      return 3.14 * s.r * s.r;
    case "square":
      return s.side * s.side;
  }
}

export function ok(value: number): Result {
  return { ok: true, value };
}

export function fail(code: string): Result {
  return { ok: false, code };
}
