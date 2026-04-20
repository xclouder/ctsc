/**
 * Phase 4 oracle (channel: diagnostics).
 *
 * Produces the semantic-diagnostics JSON that ctsc's `--check` must match
 * byte-for-byte. Mirrors the approach of oracle-binder.ts: we build a tiny
 * Program over one virtual SourceFile, run tsc's own TypeChecker, harvest
 * getSemanticDiagnostics() and serialise the subset we care about.
 *
 * Output shape (stable; fields only appended, never reordered):
 *
 *   {
 *     "diagnostics": [
 *       { "code": 2304, "category": "Error", "start": 12, "length": 3,
 *         "messageText": "Cannot find name 'foo'." }
 *     ],
 *     "tsVersion": "5.9.2"
 *   }
 *
 * Stability rules:
 * - diagnostics listed in ascending `start`, ties broken by `code`
 * - `category` is the capitalised tsc name (Error/Warning/Suggestion/Message)
 * - `messageText` is flattened via ts.flattenDiagnosticMessageText so multi-
 *   line chain messages become a single "\n"-joined string
 */

import ts from "typescript";

const VIRTUAL_FILENAME = "__ctsc_fixture__.ts";

interface DiagOut {
  code: number;
  category: string;
  start: number;
  length: number;
  messageText: string;
}

const CATEGORY_NAMES: Record<number, string> = {
  [ts.DiagnosticCategory.Warning]:    "Warning",
  [ts.DiagnosticCategory.Error]:      "Error",
  [ts.DiagnosticCategory.Suggestion]: "Suggestion",
  [ts.DiagnosticCategory.Message]:    "Message",
};

export function buildCheckerDiagJson(src: string): string {
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

  const live = program.getSourceFile(VIRTUAL_FILENAME)!;
  const diags = program.getSemanticDiagnostics(live);

  const out: DiagOut[] = [];
  for (const d of diags) {
    if (d.file !== live) continue;
    out.push({
      code: d.code,
      category: CATEGORY_NAMES[d.category] ?? `Unknown(${d.category})`,
      start: d.start ?? 0,
      length: d.length ?? 0,
      messageText: ts.flattenDiagnosticMessageText(d.messageText, "\n"),
    });
  }
  out.sort((a, b) => (a.start - b.start) || (a.code - b.code));

  return JSON.stringify({ diagnostics: out, tsVersion: ts.version });
}
