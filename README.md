# ctsc - a harness-driven C port of tsc

A long-running experiment: port Microsoft's TypeScript compiler (`tsc`) to
plain C, by running a Cursor agent in a loop that diffs our output against
`tsc`'s output and iterates until parity.

## Layout

- `upstream/TypeScript/` - shallow clone of `microsoft/TypeScript` (oracle).
- `ctsc/` - the C port (CMake + Ninja).
- `harness/` - the self-iterating harness (Node + TypeScript).
- `fixtures/` - curriculum test cases, grouped by phase/stage, prefixed by
  difficulty (`01_...`).
- `scripts/bootstrap.ps1` - one-shot setup that clones upstream, installs
  harness deps, and detects the C toolchain.
- `AGENTS.md` - non-negotiable rules for any agent editing `ctsc/`.

## Quickstart

```powershell
# 1. One-time setup
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1

# 2. Build ctsc (requires MSVC Developer PowerShell or LLVM/clang on PATH)
cmake --preset default -S ctsc
cmake --build ctsc/build/default

# 3. Sanity-check the pipeline without an agent
cd harness
npm run loop -- --dry --phase scanner --max 5

# 4. Drive the agent loop for real
npm run loop -- --phase scanner --max 20 --build
```

## Harness commands

- `npm run list`                         - list curriculum in order.
- `npm run status`                       - summary of pass/fail state.
- `npm run oracle -- <fixture-id>`       - dump tsc oracle output.
- `npm run diff   -- <fixture-id>`       - show diff between ctsc and tsc.
- `npm run loop   -- --dry`              - smoke-test without agent.
- `npm run loop`                         - real run (calls `cursor-agent`).

## Phase roadmap

- Phase 0: core (arena, UTF-8/UTF-16, hashmap, diagnostic, JSON writer) - DONE.
- Phase 1: scanner. MVP implemented; 30 fixtures; harness diffs token streams.
- Phase 2: parser. AST JSON differ + `--dump-ast`.
- Phase 3: binder.
- Phase 4: checker. The biggest phase; sub-staged: basic -> structural ->
  generics -> inference -> conditional/mapped -> control-flow narrowing.
- Phase 5: transformer + emitter (`.js` / `.d.ts` byte-equal).
- Phase 6: CLI / tsconfig / project references.

## Notes on "byte-for-byte compatible"

`tsc` has tens of thousands of baselines. Full parity is a multi-year goal,
not a single-session deliverable. The harness is designed to run indefinitely,
check in progress, and let the agent chip away at failures. See AGENTS.md for
the ground rules the agent must follow.
