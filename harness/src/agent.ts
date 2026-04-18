import { spawn } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";

import { AGENT_SESSIONS_DIR, REPO_ROOT } from "./paths.js";
import type { DiffResult, Fixture } from "./types.js";
import { readFile } from "node:fs/promises";

export interface AgentRunOptions {
  timeoutMs?: number;
  dryRun?: boolean;
  /** Override the agent CLI name (default: `agent`, i.e. Cursor CLI). */
  agentCmd?: string;
  /** Extra arguments injected between <bin> and the prompt. */
  agentArgs?: string[];
  /** Optional model override, e.g. "gpt-5" or "sonnet-4-thinking". */
  model?: string;
}

export interface AgentResult {
  invoked: boolean;
  exitCode: number;
  durationMs: number;
  stdout: string;
  stderr: string;
  promptPath: string;
  sessionDir: string;
}

/**
 * Build a focused prompt that scopes the agent to the current failure.
 * The template is deliberately concrete: agent must (1) read the TS source
 * that implements this behaviour, (2) modify ctsc only, (3) keep diff small.
 */
export async function buildPrompt(fx: Fixture, diff: DiffResult, extras: { expected: string; actual: string }): Promise<string> {
  const source = await readFile(fx.sourcePath, "utf8").catch(() => "<unreadable>");

  const referenceFile = phaseReferenceFile(fx.phase);
  const referenceNotes = phaseReferenceNotes(fx.phase);

  return `You are iterating on the ctsc project: a C port of the TypeScript compiler.
The harness runs ctsc against a fixture, diffs the output against tsc's output, and
asks you to close the gap.

Rules (AGENTS.md applies, read it first):
- Only modify files under \`ctsc/\` (and \`fixtures/\` if a test helper is strictly needed).
- Match tsc behaviour byte-for-byte. Reference \`upstream/TypeScript/src/compiler/${referenceFile}\` BEFORE editing.
- Keep the diff minimal and well-structured. Prefer extending existing modules over adding new ones.
- After edits, run: cmake --build ctsc/build/default. Fix any compile errors.
- Do NOT touch the harness, the oracle, or the differ unless explicitly asked.

Current failing fixture:
- id: ${fx.id}
- phase: ${fx.phase} / stage: ${fx.stage}
- source path: ${fx.sourcePath}

Source (${source.length} bytes):
\`\`\`ts
${source}
\`\`\`

Diff summary:
${diff.summary}

Expected output (from tsc oracle, first 2000 chars):
\`\`\`json
${extras.expected.slice(0, 2000)}
\`\`\`

Actual output (from ctsc, first 2000 chars):
\`\`\`json
${extras.actual.slice(0, 2000)}
\`\`\`

${referenceNotes}

Deliverable:
- Edits under ctsc/ that make the fixture pass.
- Do not break previously passing fixtures (the harness will re-run them).
`;
}

function phaseReferenceFile(phase: string): string {
  switch (phase) {
    case "scanner":   return "scanner.ts";
    case "parser":    return "parser.ts";
    case "binder":    return "binder.ts";
    case "checker":   return "checker.ts";
    case "emitter":   return "emitter.ts";
    default:          return "program.ts";
  }
}

function phaseReferenceNotes(phase: string): string {
  if (phase === "scanner") {
    return `Hints for scanner phase:
- TS positions are UTF-16 code unit offsets (NOT bytes, NOT UTF-8). ctsc already
  converts the source to UTF-16 via ctsc/src/core/utf8.c. Keep that invariant.
- Token kind names MUST match ts.SyntaxKind member names verbatim.
- When adding a new token, update BOTH the enum in \`ctsc/include/ctsc/scanner.h\`
  AND the name table in \`ctsc/src/scanner/token_names.c\`.
- Unicode escapes in identifiers (\\uXXXX / \\u{X}) must be decoded during
  scan; the emitted \`text\` field is the *decoded* identifier name, but
  \`start\`/\`end\` cover the raw source span.
- For '>' / '>=' / '>>' / '>>=' / '>>>' / '>>>=' the scanner emits only
  \`GreaterThanToken\`; the parser re-scans via reScanGreaterToken() when it
  needs compound forms (see scanner.c comment).
`;
  }
  if (phase === "parser") {
    return `Hints for parser phase:
- Reference: upstream/TypeScript/src/compiler/parser.ts :: Parser.parseSourceFile.
- AST node.pos = full_start (includes leading trivia). node.end = exclusive.
- For every new node kind you MUST:
  1) add the CtscSyntaxKind enum value in ctsc/include/ctsc/scanner.h,
  2) add a data struct in ctsc/include/ctsc/ast.h (CtscNode union),
  3) parse it in ctsc/src/parser/parser.c,
  4) serialize it in ctsc/src/parser/ast_json.c to match the oracle,
  5) update harness/src/oracle-ast.ts (if field shape isn't covered).
- PrefixUnaryExpression.operator is emitted as a STRING name (e.g.
  "MinusToken"), not a numeric SyntaxKind, for cross-enum parity.
`;
  }
  if (phase === "binder") {
    return `Hints for binder phase (Phase 3 - implement real binder):
- Reference: upstream/TypeScript/src/compiler/binder.ts. Start with bind(),
  bindContainer(), declareSymbol(), getSymbolFlagsForNode().
- Oracle emits {"scopes":[...], "diagnostics":[]}. See
  harness/src/oracle-binder.ts for the exact shape. Each scope entry:
  {kind, pos, end, symbols:[{name, flags[], decls:[{kind,pos,end}]}]}.
- DFS / pre-order. Symbols sorted by name. 'flags' is an array of atomic
  ts.SymbolFlags names (FunctionScopedVariable, BlockScopedVariable,
  Function, Class, Interface, TypeAlias, Enum, Property, Method,
  Parameter, ...).  Composite aliases (Value, Variable, Type, Namespace)
  are NOT emitted.
- ctsc needs new modules: ctsc/src/binder/binder.c, ctsc/src/binder/symbol.c,
  ctsc/include/ctsc/binder.h, ctsc/include/ctsc/symbol.h.  Wire them into
  ctsc/CMakeLists.txt (add_library ctsc_binder, link into ctsc + ctsc_tests).
- Replace the stub in driver main.c CMD_DUMP_BINDINGS with a real call:
      CtscParseResult r = ctsc_parse(src, src_len, &a);
      ctsc_bind(r.sourceFile, &a);
      ctsc_bindings_dump_json(r.sourceFile, &out, /*pretty*/ false);
- Start minimal: SourceFile + FunctionDeclaration locals only. The harness
  will add harder fixtures after the basics pass.
`;
  }
  if (phase === "checker") {
    return `Hints for checker phase (Phase 4 - stub).
- Oracle currently returns an empty envelope. Agent should first extend
  harness/src/oracle-checker.ts (create it) before implementing the C side.
- Reference: upstream/TypeScript/src/compiler/checker.ts. This is the largest
  file in the compiler. Do not try to implement everything; one sub-rule at
  a time driven by failing fixtures.
`;
  }
  if (phase === "emitter") {
    return `Hints for emitter phase:
- Reference: upstream/TypeScript/src/compiler/emitter.ts (Printer).
- Oracle uses ts.transpileModule with target=ES2020. Output is JS bytes
  (stdout). Line endings are LF; do NOT emit CRLF. Keep comments/blank lines
  out unless tsc would preserve them.
- Emitter must be deterministic and depend only on the AST (no extra state).
- For every new statement/expression kind you support in the parser, add the
  corresponding case to ctsc/src/emitter/emitter.c emit().
`;
  }
  return "";
}

export async function invokeAgent(fx: Fixture, prompt: string, opts: AgentRunOptions = {}): Promise<AgentResult> {
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  const sessionDir = join(AGENT_SESSIONS_DIR, `${stamp}_${safeId(fx.id)}`);
  await mkdir(sessionDir, { recursive: true });
  const promptPath = join(sessionDir, "prompt.md");
  await writeFile(promptPath, prompt, "utf8");

  if (opts.dryRun) {
    return { invoked: false, exitCode: 0, durationMs: 0, stdout: "[dry-run] agent not invoked", stderr: "", promptPath, sessionDir };
  }

  const bin = opts.agentCmd ?? process.env.CTSC_AGENT_CMD ?? "agent";
  const defaultArgs = ["-p", "--force", "--output-format", "text"];
  const extraArgs = opts.agentArgs
    ?? (process.env.CTSC_AGENT_ARGS ? process.env.CTSC_AGENT_ARGS.split(/\s+/).filter(Boolean) : defaultArgs);
  const model = opts.model ?? process.env.CTSC_AGENT_MODEL;
  const modelArgs = model ? ["--model", model] : [];
  /* The prompt is already on disk at promptPath; pass a tiny launcher prompt
   * that tells the agent where to find its real task. This keeps stdout logs
   * clean and sidesteps Windows cmdline length limits when the body is large. */
  const launcher = `Follow the instructions in ${promptPath}. Do not ask for confirmation; apply edits and build.`;
  const args = [...extraArgs, ...modelArgs, launcher];
  return await new Promise<AgentResult>((resolve) => {
    const child = spawn(bin, args, { cwd: REPO_ROOT, shell: process.platform === "win32", windowsHide: true });
    let stdout = "";
    let stderr = "";
    const started = Date.now();
    const timer = setTimeout(() => { try { child.kill("SIGKILL"); } catch { /* ignore */ } }, opts.timeoutMs ?? 30 * 60_000);
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (c: string) => { stdout += c; });
    child.stderr.on("data", (c: string) => { stderr += c; });
    child.on("close", async (code) => {
      clearTimeout(timer);
      await writeFile(join(sessionDir, "stdout.txt"), stdout, "utf8").catch(() => {});
      await writeFile(join(sessionDir, "stderr.txt"), stderr, "utf8").catch(() => {});
      resolve({ invoked: true, exitCode: code ?? -1, durationMs: Date.now() - started, stdout, stderr, promptPath, sessionDir });
    });
    child.on("error", (err) => {
      clearTimeout(timer);
      resolve({
        invoked: false,
        exitCode: -1,
        durationMs: Date.now() - started,
        stdout,
        stderr: stderr + `\n[spawn error] ${err.message}`,
        promptPath,
        sessionDir,
      });
    });
  });
}

function safeId(id: string): string {
  return id.replace(/[\\/:*?"<>|]/g, "_").slice(0, 120);
}
