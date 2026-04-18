# ctsc - Agent Guide

This repo is a long-running experiment: port Microsoft's TypeScript compiler to
plain C, driven by an agent harness that diffs our output against `tsc`'s
output and iterates until parity.

If you are an AI agent editing this repo, follow these rules strictly. The
harness will rebuild and re-test after every invocation.

## Non-negotiable rules

1. **Only modify files under `ctsc/`.** The harness (`harness/`), the Cursor
   rules (`.cursor/`), and the frozen reference checkout (`upstream/`) are
   off-limits unless the user explicitly tells you otherwise.
2. **Match `tsc` byte-for-byte.** We mirror `ts.SyntaxKind` names, UTF-16
   positions, and message text verbatim. Do not "improve" on tsc's behaviour.
3. **Read the reference first.** Before editing any phase, open the
   corresponding file under `upstream/TypeScript/src/compiler/` and cite the
   lines you are mirroring in your commit/PR message.
4. **Keep diffs minimal.** Extend existing modules; do not add new top-level
   directories without reason.
5. **Build + test after every edit.** `cmake --build ctsc/build/default`
   must succeed, and `ctsc/build/default/ctsc_tests` must pass. If a test
   becomes outdated, update it; never delete tests.
6. **Write new tests for new behaviour.** Every new scanner token / AST node /
   checker rule must have a unit test under `ctsc/tests/`.
7. **Preserve UTF-16 offsets.** tsc reports positions in UTF-16 code units.
   `ctsc_utf16_from_utf8` already converts the input; keep `scanner.pos` as a
   UTF-16 index.

## Architecture snapshot

- `ctsc/include/ctsc/` - public headers. Add new headers here (e.g. `parser.h`,
  `ast.h`).
- `ctsc/src/core/` - allocator, string ref, hashmap, diagnostic list, UTF-8,
  JSON writer, file IO. Keep these dependency-free.
- `ctsc/src/scanner/` - scanner and token name table.
- `ctsc/src/parser/`, `binder/`, `checker/`, `transformer/`, `emitter/`,
  `driver/` - will be added as the curriculum unlocks them.

## Harness contract

Your prompt will include:
- the failing fixture source,
- tsc's expected JSON output,
- ctsc's actual JSON output,
- a one-line diff summary.

Your job: edit `ctsc/` so the next run of the same fixture matches tsc. The
harness will re-run ALL previously green fixtures; do not regress them.

## When a TS/JS feature does not map cleanly to C

- Record the decision in `docs/semantics-notes.md` (create it if absent).
- Prefer fidelity over convenience. Example: JS strings are UTF-16 code unit
  sequences; do NOT silently "optimise" to UTF-8 byte offsets.

## Commit messages

Use imperative mood. Reference the TypeScript source you mirrored, e.g.

```
scanner: handle BigInt literal suffix (n)

Mirrors upstream/TypeScript/src/compiler/scanner.ts:~3070 (scanBigIntLiteral).
```
