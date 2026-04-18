import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile, stat } from "node:fs/promises";
import { dirname, join } from "node:path";
import ts from "typescript";

import { ORACLE_CACHE_DIR } from "./paths.js";
import type { Fixture, OracleArtifacts } from "./types.js";
import { buildAstJson } from "./oracle-ast.js";
import { buildBindingsJson } from "./oracle-binder.js";

/*
 * ts.SyntaxKind contains range-marker aliases (FirstAssignment, FirstKeyword,
 * FirstLiteralToken, ...) that share numeric values with real tokens. The
 * reverse-lookup `ts.SyntaxKind[kind]` returns whichever name was assigned
 * last, which is frequently the alias. We build an explicit canonical-name
 * table that prefers real token names so the oracle output lines up with
 * ctsc's name table (ctsc/src/scanner/token_names.c).
 */
const SYNTHETIC_PREFIXES = ["First", "Last"];
function buildCanonicalNameTable(): Map<number, string> {
  const m = new Map<number, string>();
  for (const key of Object.keys(ts.SyntaxKind)) {
    if (/^\d+$/.test(key)) continue; // reverse entries
    const value = (ts.SyntaxKind as any)[key] as number;
    const isAlias = SYNTHETIC_PREFIXES.some((p) => key.startsWith(p));
    const existing = m.get(value);
    if (existing === undefined) {
      m.set(value, key);
      continue;
    }
    const existingIsAlias = SYNTHETIC_PREFIXES.some((p) => existing.startsWith(p));
    if (existingIsAlias && !isAlias) {
      m.set(value, key);
    }
  }
  return m;
}

const CANONICAL_KIND_NAMES = buildCanonicalNameTable();

export function kindName(k: ts.SyntaxKind): string {
  return CANONICAL_KIND_NAMES.get(k) ?? ts.SyntaxKind[k] ?? `Unknown(${k})`;
}

export interface OracleKey {
  phase: string;
  sourceHash: string;
  tsVersion: string;
}

function hashSource(src: string): string {
  return createHash("sha256").update(src).digest("hex").slice(0, 16);
}

export function oracleKey(fx: Fixture, src: string): OracleKey {
  return {
    phase: fx.phase,
    sourceHash: hashSource(src),
    tsVersion: ts.version,
  };
}

function cachePath(key: OracleKey, file: string): string {
  return join(ORACLE_CACHE_DIR, key.phase, `${key.tsVersion}_${key.sourceHash}`, file);
}

async function exists(p: string): Promise<boolean> {
  try { await stat(p); return true; } catch { return false; }
}

/**
 * Produce the scanner token stream in the exact JSON shape that
 * ctsc's `--dump-tokens` emits. Keep format parity in lockstep with
 * ctsc/src/scanner/scanner.c :: ctsc_scanner_dump_tokens_json().
 */
export function buildTokensJson(src: string): string {
  const scanner = ts.createScanner(
    ts.ScriptTarget.Latest,
    /*skipTrivia*/ true,
    ts.LanguageVariant.Standard,
    src,
  );
  const tokens: any[] = [];
  const diagnostics: any[] = [];
  scanner.setOnError((message, length) => {
    diagnostics.push({
      code: message.code,
      start: scanner.getTokenStart(),
      length,
      category: message.category,
      message: ts.flattenDiagnosticMessageText(message.message, "\n"),
    });
  });
  while (true) {
    const kind = scanner.scan();
    const start = scanner.getTokenStart();
    const end = scanner.getTokenEnd();
    const tok: any = {
      kind: kindName(kind),
      start,
      end,
    };
    if (
      kind === ts.SyntaxKind.Identifier ||
      kind === ts.SyntaxKind.NumericLiteral ||
      kind === ts.SyntaxKind.BigIntLiteral ||
      kind === ts.SyntaxKind.PrivateIdentifier ||
      (kind >= ts.SyntaxKind.FirstKeyword && kind <= ts.SyntaxKind.LastKeyword)
    ) {
      tok.text = scanner.getTokenText();
    }
    if (kind === ts.SyntaxKind.StringLiteral || kind === ts.SyntaxKind.NoSubstitutionTemplateLiteral) {
      tok.text = scanner.getTokenText();
      tok.value = scanner.getTokenValue();
    }
    if (kind === ts.SyntaxKind.EndOfFileToken) {
      tokens.push(tok);
      break;
    }
    tokens.push(tok);
  }
  return JSON.stringify({ tokens, diagnostics });
}

export async function getOracle(fx: Fixture, src: string): Promise<OracleArtifacts> {
  const key = oracleKey(fx, src);
  const out: OracleArtifacts = {};
  if (fx.phase === "scanner") {
    const file = cachePath(key, "tokens.json");
    if (await exists(file)) {
      out.tokensJson = await readFile(file, "utf8");
    } else {
      const json = buildTokensJson(src);
      await mkdir(dirname(file), { recursive: true });
      await writeFile(file, json, "utf8");
      out.tokensJson = json;
    }
    return out;
  }
  if (fx.phase === "parser") {
    const file = cachePath(key, "ast.json");
    if (await exists(file)) {
      out.astJson = await readFile(file, "utf8");
    } else {
      const json = buildAstJson(src);
      await mkdir(dirname(file), { recursive: true });
      await writeFile(file, json, "utf8");
      out.astJson = json;
    }
    return out;
  }
  if (fx.phase === "binder") {
    const file = cachePath(key, "bindings.json");
    if (await exists(file)) {
      out.tokensJson = await readFile(file, "utf8");
    } else {
      const json = buildBindingsJson(src);
      await mkdir(dirname(file), { recursive: true });
      await writeFile(file, json, "utf8");
      out.tokensJson = json;
    }
    return out;
  }
  if (fx.phase === "checker") {
    const json = JSON.stringify({ types: [], diagnostics: [] });
    out.tokensJson = json;
    return out;
  }
  if (fx.phase === "emitter") {
    // For Phase 5 we will use ts.transpileModule as the JS baseline.
    // Agent will replace this with a proper multi-output oracle
    // (emit.js + emit.d.ts + diagnostics.txt).
    const ts = (await import("typescript")).default;
    const out2 = ts.transpileModule(src, { compilerOptions: { target: ts.ScriptTarget.ES2020 } });
    out.emitJs = out2.outputText;
    return out;
  }
  return out;
}
