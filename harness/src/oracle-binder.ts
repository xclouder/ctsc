/**
 * Phase 3 oracle: walk tsc's bound SourceFile and emit a hierarchical
 * symbol-table JSON that ctsc's `--dump-bindings` must match byte-for-byte.
 *
 * Output shape (stable, minimal; we can extend later without breaking parity
 * by adding new *fields* — never reorder existing ones):
 *
 *   {
 *     "scopes": [
 *       { "kind": "SourceFile",         "pos": 0,  "end": 42,
 *         "symbols": [
 *           { "name": "foo", "flags": ["FunctionScopedVariable"],
 *             "decls": [ { "kind": "VariableDeclaration", "pos": 4, "end": 9 } ] }
 *         ]
 *       },
 *       { "kind": "FunctionDeclaration", "pos": 12, "end": 38, "symbols": [...] }
 *     ],
 *     "diagnostics": []
 *   }
 *
 * We rely on tsc's internal `node.locals` SymbolTable (populated by the binder).
 * To trigger binding we build a tiny Program and ask for the TypeChecker.
 *
 * Stability rules:
 * - `scopes` order is DFS / pre-order (same as tsc's own traversal).
 * - `symbols` are sorted by name (ascending, Unicode code-point).
 * - `flags` lists only *atomic* (single-bit) ts.SymbolFlags names, sorted by
 *   numeric value. This avoids ambiguity from composite aliases like
 *   `Value = Variable | Property | ...`.
 * - Positions are `node.pos` / `node.end` (UTF-16 code units), matching the
 *   parser oracle.
 */

import ts from "typescript";
import { kindName } from "./oracle.js";

const VIRTUAL_FILENAME = "__ctsc_fixture__.ts";

/** Build the atomic-flags table once, lazily. */
let ATOMIC_SYMBOL_FLAGS: [number, string][] | null = null;
function atomicSymbolFlags(): [number, string][] {
  if (ATOMIC_SYMBOL_FLAGS) return ATOMIC_SYMBOL_FLAGS;
  const out: [number, string][] = [];
  for (const key of Object.keys(ts.SymbolFlags)) {
    if (/^\d+$/.test(key)) continue;
    const v = (ts.SymbolFlags as unknown as Record<string, number>)[key];
    if (typeof v !== "number" || v === 0) continue;
    /* Only keep single-bit flags so composite aliases (Variable, Value,
     * Type, Namespace, ...) don't produce overlapping names. */
    if ((v & (v - 1)) === 0) out.push([v, key]);
  }
  out.sort((a, b) => a[0] - b[0]);
  ATOMIC_SYMBOL_FLAGS = out;
  return out;
}

function flagsNames(flags: number): string[] {
  const names: string[] = [];
  for (const [v, name] of atomicSymbolFlags()) {
    if ((flags & v) !== 0) names.push(name);
  }
  return names;
}

interface ScopeOut {
  kind: string;
  pos: number;
  end: number;
  symbols: Array<{
    name: string;
    flags: string[];
    decls: Array<{ kind: string; pos: number; end: number }>;
  }>;
}

function buildProgram(src: string): ts.SourceFile {
  const sf = ts.createSourceFile(
    VIRTUAL_FILENAME,
    src,
    ts.ScriptTarget.Latest,
    /*setParentNodes*/ true,
    ts.ScriptKind.TS,
  );
  const host: ts.CompilerHost = {
    fileExists: (f) => f === VIRTUAL_FILENAME,
    readFile: (f) => (f === VIRTUAL_FILENAME ? src : undefined),
    getSourceFile: (f) => (f === VIRTUAL_FILENAME ? sf : undefined),
    getCanonicalFileName: (f) => f,
    useCaseSensitiveFileNames: () => true,
    getNewLine: () => "\n",
    getDefaultLibFileName: () => "lib.d.ts",
    getCurrentDirectory: () => "/",
    writeFile: () => { /* noop */ },
  };
  const program = ts.createProgram({
    rootNames: [VIRTUAL_FILENAME],
    options: {
      target: ts.ScriptTarget.Latest,
      noEmit: true,
      noResolve: true,
      noLib: true,
      allowJs: false,
      skipLibCheck: true,
      skipDefaultLibCheck: true,
    },
    host,
  });
  /* Touching the checker forces the binder to run on all SourceFiles. */
  program.getTypeChecker();
  return program.getSourceFile(VIRTUAL_FILENAME)!;
}

export function buildBindingsJson(src: string): string {
  const sf = buildProgram(src);
  const scopes: ScopeOut[] = [];

  function visit(node: ts.Node): void {
    const locals = (node as unknown as { locals?: ts.SymbolTable }).locals;
    if (locals !== undefined) {
      const symbols: ScopeOut["symbols"] = [];
      locals.forEach((sym, nameKey) => {
        const decls = (sym.declarations ?? []).map((d) => ({
          kind: kindName(d.kind),
          pos: d.pos,
          end: d.end,
        }));
        symbols.push({
          name: String(nameKey),
          flags: flagsNames(sym.flags),
          decls,
        });
      });
      symbols.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
      scopes.push({
        kind: kindName(node.kind),
        pos: node.pos,
        end: node.end,
        symbols,
      });
    }
    ts.forEachChild(node, visit);
  }

  visit(sf);
  return JSON.stringify({ scopes, diagnostics: [] });
}
