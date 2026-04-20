/**
 * Phase 4 oracle (channel: types).
 *
 * For each typed-declaration name node in the source, record the inferred
 * type string as produced by ts.TypeChecker.typeToString. ctsc's
 * `--dump-types` must produce the same JSON byte-for-byte.
 *
 * Output shape:
 *
 *   {
 *     "entries": [
 *       { "name": "x", "kind": "VariableDeclaration",
 *         "pos": 10, "end": 15, "type": "number" }
 *     ],
 *     "tsVersion": "5.9.2"
 *   }
 *
 * Stability rules:
 * - entries sorted by [pos, end] then kind, then name
 * - kind is the SyntaxKind canonical name of the *declaration* node
 * - only declarations we can realistically infer in M4.0 are dumped:
 *     VariableDeclaration, ParameterDeclaration, FunctionDeclaration,
 *     MethodDeclaration, PropertyDeclaration, PropertySignature.
 *   (M4.1+ will extend the set.)
 * - `type` string formatting uses NoTruncation | UseFullyQualifiedType;
 *   otherwise the tsc defaults apply, which is what we aim to mirror.
 */

import ts from "typescript";
import { kindName } from "./oracle.js";

const VIRTUAL_FILENAME = "__ctsc_fixture__.ts";

interface TypeOut {
  name: string;
  kind: string;
  pos: number;
  end: number;
  type: string;
}

const DECL_KINDS_M40 = new Set<ts.SyntaxKind>([
  ts.SyntaxKind.VariableDeclaration,
  ts.SyntaxKind.Parameter,
  ts.SyntaxKind.FunctionDeclaration,
  ts.SyntaxKind.MethodDeclaration,
  ts.SyntaxKind.PropertyDeclaration,
  ts.SyntaxKind.PropertySignature,
]);

function nameOf(decl: ts.Declaration): { name: string; nameNode: ts.Node } | null {
  const anyDecl = decl as any;
  const n = anyDecl.name as ts.Node | undefined;
  if (!n) return null;
  if (ts.isIdentifier(n)) return { name: n.text, nameNode: n };
  if (ts.isStringLiteral(n) || ts.isNumericLiteral(n)) return { name: n.text, nameNode: n };
  // ComputedPropertyName, BindingPattern: skip in M4.0
  return null;
}

export function buildCheckerTypesJson(src: string): string {
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
  const checker = program.getTypeChecker();

  const formatFlags = ts.TypeFormatFlags.NoTruncation | ts.TypeFormatFlags.UseFullyQualifiedType;
  const entries: TypeOut[] = [];

  function visit(node: ts.Node): void {
    if (DECL_KINDS_M40.has(node.kind)) {
      const info = nameOf(node as ts.Declaration);
      if (info) {
        const t = checker.getTypeAtLocation(info.nameNode);
        entries.push({
          name: info.name,
          kind: kindName(node.kind),
          pos: (info.nameNode as any).pos as number,
          end: (info.nameNode as any).end as number,
          type: checker.typeToString(t, info.nameNode, formatFlags),
        });
      }
    }
    ts.forEachChild(node, visit);
  }
  visit(live);

  entries.sort((a, b) =>
    (a.pos - b.pos) ||
    (a.end - b.end) ||
    (a.kind < b.kind ? -1 : a.kind > b.kind ? 1 : 0) ||
    (a.name < b.name ? -1 : a.name > b.name ? 1 : 0),
  );

  return JSON.stringify({ entries, tsVersion: ts.version });
}
