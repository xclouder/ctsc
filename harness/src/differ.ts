import type { DiffResult } from "./types.js";

interface TokenRecord {
  kind: string;
  start: number;
  end: number;
  text?: string;
  value?: string;
}

interface TokenDoc {
  tokens: TokenRecord[];
  diagnostics: any[];
}

function tryParse(label: string, raw: string): TokenDoc | string {
  try {
    const v = JSON.parse(raw);
    if (!v || !Array.isArray(v.tokens)) return `${label}: invalid token doc (no tokens array)`;
    return v as TokenDoc;
  } catch (e: any) {
    return `${label}: JSON parse error - ${e.message}\n---\n${raw.slice(0, 400)}`;
  }
}

function fmtTok(t: TokenRecord): string {
  const base = `${t.kind}[${t.start}..${t.end}]`;
  if (t.text !== undefined) return `${base} text=${JSON.stringify(t.text)}`;
  return base;
}

export function diffTokens(expected: string, actual: string): DiffResult {
  const exp = tryParse("expected", expected);
  if (typeof exp === "string") return { equal: false, summary: exp };
  const act = tryParse("actual", actual);
  if (typeof act === "string") return { equal: false, summary: act };

  const n = Math.max(exp.tokens.length, act.tokens.length);
  for (let i = 0; i < n; i++) {
    const e = exp.tokens[i];
    const a = act.tokens[i];
    if (!e) {
      return {
        equal: false,
        summary: `extra token at index ${i}: ${fmtTok(a!)}`,
        firstMismatch: { path: `tokens[${i}]`, expected: "<none>", actual: fmtTok(a!) },
      };
    }
    if (!a) {
      return {
        equal: false,
        summary: `missing token at index ${i}: expected ${fmtTok(e)}`,
        firstMismatch: { path: `tokens[${i}]`, expected: fmtTok(e), actual: "<none>" },
      };
    }
    if (e.kind !== a.kind || e.start !== a.start || e.end !== a.end) {
      return {
        equal: false,
        summary: `token ${i} mismatch: expected ${fmtTok(e)} got ${fmtTok(a)}`,
        firstMismatch: { path: `tokens[${i}]`, expected: fmtTok(e), actual: fmtTok(a) },
      };
    }
    if ((e.text ?? "") !== (a.text ?? "")) {
      return {
        equal: false,
        summary: `token ${i} text mismatch: expected ${JSON.stringify(e.text)} got ${JSON.stringify(a.text)}`,
        firstMismatch: { path: `tokens[${i}].text`, expected: JSON.stringify(e.text ?? null), actual: JSON.stringify(a.text ?? null) },
      };
    }
    if ((e.value ?? "") !== (a.value ?? "")) {
      return {
        equal: false,
        summary: `token ${i} value mismatch: expected ${JSON.stringify(e.value)} got ${JSON.stringify(a.value)}`,
        firstMismatch: { path: `tokens[${i}].value`, expected: JSON.stringify(e.value ?? null), actual: JSON.stringify(a.value ?? null) },
      };
    }
  }
  return { equal: true, summary: `OK (${exp.tokens.length} tokens)` };
}

export function diffAst(expected: string, actual: string): DiffResult {
  let exp: any; let act: any;
  try { exp = JSON.parse(expected); } catch (e: any) { return { equal: false, summary: `expected AST JSON parse error: ${e.message}` }; }
  try { act = JSON.parse(actual);   } catch (e: any) { return { equal: false, summary: `actual AST JSON parse error: ${e.message}` }; }
  return diffAstNode(exp, act, "$");
}

function shortVal(v: unknown): string {
  if (v == null) return String(v);
  if (typeof v === "string") return JSON.stringify(v.length > 80 ? v.slice(0, 80) + "..." : v);
  if (typeof v === "number" || typeof v === "boolean") return String(v);
  if (Array.isArray(v)) return `Array(${v.length})`;
  if (typeof v === "object") {
    const o = v as any;
    return `{kind:${o.kind ?? "?"}}`;
  }
  return String(v);
}

function diffAstNode(exp: any, act: any, path: string): DiffResult {
  if (exp == null && act == null) return { equal: true, summary: "" };
  if (exp == null || act == null) {
    return { equal: false, summary: `${path}: null mismatch (expected=${shortVal(exp)} actual=${shortVal(act)})`,
             firstMismatch: { path, expected: shortVal(exp), actual: shortVal(act) } };
  }
  if (typeof exp !== "object" || typeof act !== "object") {
    if (exp !== act) {
      return { equal: false, summary: `${path}: value mismatch`,
               firstMismatch: { path, expected: shortVal(exp), actual: shortVal(act) } };
    }
    return { equal: true, summary: "" };
  }
  if (Array.isArray(exp) || Array.isArray(act)) {
    if (!Array.isArray(exp) || !Array.isArray(act)) {
      return { equal: false, summary: `${path}: array/object shape mismatch`,
               firstMismatch: { path, expected: shortVal(exp), actual: shortVal(act) } };
    }
    const n = Math.max(exp.length, act.length);
    for (let i = 0; i < n; i++) {
      const r = diffAstNode(exp[i], act[i], `${path}[${i}]`);
      if (!r.equal) return r;
    }
    return { equal: true, summary: "" };
  }
  const keys = new Set<string>([...Object.keys(exp), ...Object.keys(act)]);
  // stable key order for deterministic diff; prefer tsc-canonical key order
  const ordered = [...keys].sort();
  for (const k of ordered) {
    const r = diffAstNode(exp[k], act[k], `${path}.${k}`);
    if (!r.equal) return r;
  }
  return { equal: true, summary: `OK (AST kind=${exp.kind})` };
}

export function diffText(expected: string, actual: string): DiffResult {
  if (expected === actual) return { equal: true, summary: `OK (${expected.length} bytes)` };
  // find first differing char to give a pointer hint
  const n = Math.min(expected.length, actual.length);
  let i = 0; while (i < n && expected[i] === actual[i]) i++;
  const ctxStart = Math.max(0, i - 20);
  const exCtx = expected.slice(ctxStart, i + 20);
  const acCtx = actual.slice(ctxStart, i + 20);
  return {
    equal: false,
    summary: `text mismatch at offset ${i} (expected ${expected.length}b, actual ${actual.length}b)`,
    firstMismatch: { path: `@${i}`, expected: JSON.stringify(exCtx), actual: JSON.stringify(acCtx) },
  };
}

export function diffPhase(phase: string, expected: string, actual: string): DiffResult {
  if (phase === "scanner") return diffTokens(expected, actual);
  if (phase === "parser")  return diffAst(expected, actual);
  if (phase === "binder" || phase === "checker") return diffAst(expected, actual);
  if (phase === "emitter") return diffText(expected, actual);
  return { equal: false, summary: `differ for phase '${phase}' not implemented yet` };
}
