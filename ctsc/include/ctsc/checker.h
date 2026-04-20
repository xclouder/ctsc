#ifndef CTSC_CHECKER_H
#define CTSC_CHECKER_H

#include "common.h"
#include "ast.h"
#include "binder.h"
#include "type.h"
#include "buffer.h"

/*
 * Phase 4: checker.
 *
 * Mirrors upstream/TypeScript/src/compiler/checker.ts. M4.0 implements only
 * the small slice needed to match the oracle for the curriculum fixtures:
 *
 *   types channel (→ ctsc_check_dump_types_json):
 *     - for each VariableDeclaration / ParameterDeclaration /
 *       FunctionDeclaration / MethodDeclaration / PropertyDeclaration /
 *       PropertySignature carrying an Identifier name, record the inferred
 *       or annotated type. Output JSON matches harness/src/
 *       oracle-checker-types.ts buildCheckerTypesJson().
 *
 *   diag channel (→ ctsc_check_dump_diag_json):
 *     - TS2304 "Cannot find name '...'" for unresolved Identifier references
 *     - TS2322 "Type '...' is not assignable to type '...'." when a
 *       VariableDeclaration has a type annotation incompatible with its
 *       initializer (span on the binding name, matching tsc)
 *     - TS2345 "Argument of type '...' is not assignable to parameter of type
 *       '...'." for a simple call to a FunctionDeclaration when an argument
 *       expression is not assignable (span on the argument node; mirrors
 *       checker.ts getSignatureApplicabilityError ~36181)
 *     - TS2554 "Expected N arguments, but got M." when a call to a resolved
 *       FunctionDeclaration supplies fewer than the minimum arity (span on
 *       the callee expression; mirrors checker.ts getArgumentArityError
 *       ~36410-36477 and getDiagnosticSpanForCallNode ~36359-36362)
 *     - TS2339 "Property '...' does not exist on type '...'." when a property
 *       access targets a missing member of an anonymous object literal type
 *       (span on the property name; mirrors checker.ts checkPropertyAccessExpression
 *       / reportNonexistentProperty path ~34948-34968)
 *     - TS2552 (basic) "Cannot find name '...'. Did you mean 'y'?" optional,
 *       guarded behind a Levenshtein <=2 suggestion against in-scope symbols
 *
 * Both channels walk the AST once, reuse binder's CtscBindResult to resolve
 * names, and accumulate their respective output arrays. A single pass runs
 * both, and the CLI picks which JSON to emit.
 */

typedef struct {
    int         code;           /* TS2304, TS2552, ... */
    const char* category;       /* "Error" / "Warning" / ... */
    int         start;          /* UTF-16 code unit offset */
    int         length;
    /* messageText stored as UTF-8 (message text is ASCII in the M4.0 set). */
    const char* message;
} CtscCheckDiagnostic;

typedef struct {
    /* Name node text (UTF-16); borrowed from source / arena. */
    const uint16_t* name;
    size_t          name_len;
    /* SyntaxKind name of the enclosing declaration (e.g. "VariableDeclaration"). */
    const char*     decl_kind_name;
    /* pos / end of the *name* identifier (matches oracle). */
    int             pos;
    int             end;
    /* One of {type, type_string} is populated:
     *   - `type`: structural CtscType, formatted on demand.
     *   - `type_string`: pre-formatted UTF-8 text (for function types etc.
     *     that the M4.0 registry does not yet model). */
    CtscType*       type;
    const char*     type_string;
    size_t          type_string_len;
} CtscCheckTypeEntry;

typedef struct {
    CtscCheckDiagnostic* diagnostics;
    size_t               diagnostics_len;
    size_t               diagnostics_cap;

    CtscCheckTypeEntry*  entries;
    size_t               entries_len;
    size_t               entries_cap;

    CtscTypeRegistry     registry;
} CtscCheckResult;

struct CtscArena;

/* Run the M4.0 checker over `sourceFile`. `binding` is the binder output for
 * the same tree (ctsc_bind). Allocates everything inside `arena`. */
CtscCheckResult* ctsc_check(const CtscNode* sourceFile, CtscBindResult* binding,
                            struct CtscArena* arena);

/* Dump diagnostics JSON matching harness/src/oracle-checker-diag.ts. */
void ctsc_check_dump_diag_json(CtscCheckResult* r, CtscBuffer* out, bool pretty);

/* Dump types JSON matching harness/src/oracle-checker-types.ts. */
void ctsc_check_dump_types_json(CtscCheckResult* r, CtscBuffer* out, bool pretty);

#endif
