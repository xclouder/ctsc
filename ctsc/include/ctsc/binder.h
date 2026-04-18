#ifndef CTSC_BINDER_H
#define CTSC_BINDER_H

#include "common.h"
#include "ast.h"
#include "symbol.h"
#include "buffer.h"

/*
 * Phase 3: binder.
 *
 * Mirrors the role of upstream/TypeScript/src/compiler/binder.ts ::
 * bindSourceFile / bindContainer / declareSymbol. We do not yet replicate
 * ts.Symbol / ts.SymbolTable verbatim; instead we grow a minimal model in
 * lockstep with harness/src/oracle-binder.ts (which reads tsc's node.locals
 * and serialises a stable JSON envelope).
 *
 * Output contract (see oracle-binder.ts buildBindingsJson):
 *   { "scopes": [ {kind, pos, end, symbols:[{name,flags,decls}]} ... ],
 *     "diagnostics": [] }
 *
 * scope entries are in DFS / pre-order. Symbols within a scope are sorted
 * ascending by name (Unicode code-point).
 */

typedef struct CtscScope {
    /* The AST container node that owns this scope (SourceFile,
     * FunctionDeclaration, Block w/ block-scoped decl, ...). */
    const CtscNode* node;
    CtscSymbolTable locals;
} CtscScope;

typedef struct CtscBindResult {
    CtscScope** scopes;
    size_t      scopes_len;
    size_t      scopes_cap;
} CtscBindResult;

struct CtscArena;

/*
 * Walk `sourceFile` and populate a CtscBindResult attached to the arena.
 * Declares symbols for containers the binder currently recognises; starts
 * minimal (SourceFile only) and grows as fixtures unlock new declaration
 * kinds. Must not allocate outside `arena`.
 */
CtscBindResult* ctsc_bind(const CtscNode* sourceFile, struct CtscArena* arena);

/*
 * Serialise the binder result to JSON matching harness/src/oracle-binder.ts.
 */
void ctsc_bindings_dump_json(const CtscNode* sourceFile, CtscBuffer* out, bool pretty);

#endif
