// Bug B17: function return type is a union of object types.
// ctsc parser closed the function body early after the first object type
// because it didn't continue parsing `|` as a union-type continuation
// at function-return-type position. The function body was then re-emitted
// as a top-level block, producing `SyntaxError: Illegal return statement`.
//
// Expected (tsc transpileModule, ES2020): the union return type is fully
// erased, the function body is preserved verbatim.

export function safeParse(s: string): { ok: true; value: number } | { ok: false; code: string } {
  try {
    const n = Number(s);
    if (Number.isNaN(n)) {
      return { ok: false, code: "NaN" };
    }
    return { ok: true, value: n };
  } catch {
    return { ok: false, code: "ERR" };
  }
}

export function classify(x: number): { kind: "zero" } | { kind: "pos"; value: number } | { kind: "neg"; value: number } {
  if (x === 0) return { kind: "zero" };
  if (x > 0) return { kind: "pos", value: x };
  return { kind: "neg", value: x };
}
