#include "ctsc/checker.h"
#include "ctsc/parser.h"
#include "ctsc/binder.h"
#include "ctsc/arena.h"
#include "ctsc/buffer.h"
#include "ctsc/type.h"

#include <stdio.h>
#include <string.h>

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)

int test_checker(void) {
    int failed = 0;

    /*
     * TS2322 on the binding identifier for incompatible annotation vs initializer.
     * Mirrors fixtures under checker/assignability and tsc's span on the name
     * (createDiagnosticForNode on the VariableDeclaration name).
     */
    {
        const char* src = "// @checker: diag\r\nconst x: string = 42;\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2322);
            EXPECT(cr->diagnostics[0].start == 25);
            EXPECT(cr->diagnostics[0].length == 1);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Type 'number' is not assignable to type 'string'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    {
        const char* src = "// @checker: diag\r\nconst x: number = \"hi\";\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2322);
            EXPECT(cr->diagnostics[0].start == 25);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Type 'string' is not assignable to type 'number'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    {
        const char* src = "// @checker: diag\r\nconst x: number = true;\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2322);
            EXPECT(cr->diagnostics[0].start == 25);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Type 'boolean' is not assignable to type 'number'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /* Number literal types: mismatch is TS2322 (checker.ts isSimpleTypeRelatedTo ~22240-22245). */
    {
        const char* src = "// @checker: diag\r\nconst x: 1 = 2;\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2322);
            EXPECT(cr->diagnostics[0].start == 25);
            EXPECT(cr->diagnostics[0].length == 1);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Type '2' is not assignable to type '1'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /*
     * Object literal assigned to an interface: TS2322 on the incompatible
     * property initializer with widened primitive message (checker.ts
     * checkPropertyAssignment ~41503 / checkVariableDeclaration ~45282).
     * Mirrors fixtures/checker/interfaces/03_interface_mismatch.ts (LF).
     */
    {
        const char* src = "// @checker: diag\ninterface P {\n  a: number;\n}\nconst p: P = { a: \"x\" };\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2322);
            /* tsc highlights the property NAME, not the value (the decoded
             * `"x"` content sat at column 66 under the old value-span
             * behavior). With the key-span fix the span covers `a`. */
            EXPECT(cr->diagnostics[0].start == 62);
            EXPECT(cr->diagnostics[0].length == 1);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Type 'string' is not assignable to type 'number'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /* String literal types: incompatible literals are TS2322; message keeps quoted literals (checker.ts ~22238). */
    {
        const char* src = "// @checker: diag\r\nconst x: \"a\" = \"b\";\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2322);
            EXPECT(cr->diagnostics[0].start == 25);
            EXPECT(cr->diagnostics[0].length == 1);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Type '\"b\"' is not assignable to type '\"a\"'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /*
     * TS2345 on the argument expression when a call targets a FunctionDeclaration
     * and the argument is not assignable (checker.ts getSignatureApplicabilityError
     * ~36181). Mirrors fixtures/checker/function_calls/01_arg_type_mismatch.ts (CRLF).
     */
    {
        const char* src = "// @checker: diag\r\nfunction f(x: number) {}\r\nf(\"hi\");\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2345);
            EXPECT(cr->diagnostics[0].start == 47);
            EXPECT(cr->diagnostics[0].length == 4);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Argument of type 'string' is not assignable to parameter of type 'number'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /*
     * TS2345 when a call argument violates a generic parameter's `extends`
     * constraint. Mirrors checker.ts inferTypeArguments +
     * getSignatureApplicabilityError (~35827/36181): the inferred type
     * argument fails `T extends C`, so the paramType in the applicability
     * error collapses to C and the argument's (widened) type is reported as
     * `source`. Fixture: checker/generic_constraints/02_extends_violation.ts.
     */
    {
        const char* src =
            "// @checker: diag\r\n"
            "function len<T extends { length: number }>(x: T): number {\r\n"
            "  return x.length;\r\n"
            "}\r\n"
            "const r = len(42);\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2345);
            /* start/length match tsc's UTF-16 span on the `42` argument. */
            EXPECT(cr->diagnostics[0].length == 2);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Argument of type 'number' is not assignable to parameter of type '{ length: number; }'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /*
     * Class generic constraint with a TypeLiteral `{ ... }` type argument.
     * Mirrors harness/src/oracle-checker-types.ts ts.forEachChild descent
     * into ClassDeclaration.typeParameters + VariableDeclaration.type →
     * TypeReference.typeArguments → TypeLiteral members. Fixture:
     * checker/generic_constraints/04_class_generic_constraint.ts. Verifies:
     *   - PropertySignature entry from the `extends { id: number }` constraint
     *   - TypeLiteral type argument `{ id; name }` preserved on the reference
     *     (c's type stringifies as `Container<{ id: number; name: string; }>`)
     *   - inner PropertySignature entries from the TypeLiteral type argument
     */
    {
        const char* src =
            "// @checker: types\n"
            "class Container<T extends { id: number }> {\n"
            "  item: T;\n"
            "}\n"
            "declare const c: Container<{ id: number; name: string }>;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        bool saw_constraint_id = false;
        bool saw_arg_id = false;
        bool saw_arg_name = false;
        bool saw_c = false;
        for (size_t i = 0; i < cr->entries_len; ++i) {
            const CtscCheckTypeEntry* e = &cr->entries[i];
            if (!e->decl_kind_name) continue;
            if (strcmp(e->decl_kind_name, "PropertySignature") == 0
                && e->name_len == 2 && e->name[0] == 'i' && e->name[1] == 'd') {
                if (e->pos < 60) saw_constraint_id = true; else saw_arg_id = true;
            }
            if (strcmp(e->decl_kind_name, "PropertySignature") == 0
                && e->name_len == 4 && e->name[0] == 'n' && e->name[1] == 'a'
                && e->name[2] == 'm' && e->name[3] == 'e') {
                saw_arg_name = true;
            }
            if (strcmp(e->decl_kind_name, "VariableDeclaration") == 0
                && e->name_len == 1 && e->name[0] == 'c') {
                saw_c = true;
                CtscBuffer ts;
                ctsc_buf_init(&ts);
                ctsc_type_to_string(e->type, &ts);
                EXPECT(ts.len == strlen("Container<{ id: number; name: string; }>")
                       && memcmp(ts.data, "Container<{ id: number; name: string; }>",
                                 strlen("Container<{ id: number; name: string; }>")) == 0);
                ctsc_buf_free(&ts);
            }
        }
        EXPECT(saw_constraint_id);
        EXPECT(saw_arg_id);
        EXPECT(saw_arg_name);
        EXPECT(saw_c);
        ctsc_arena_free(&a);
    }

    /*
     * Generic function with a TypeLiteral return annotation `{ k: K; v: V }`
     * where one type parameter is constrained (`K extends string`) and the
     * other is unconstrained (`V`). Mirrors
     * harness/src/oracle-checker-types.ts ts.forEachChild descent into
     * FunctionDeclaration.type → TypeLiteral members, and checker.ts
     * inferTypeArguments (~35827) + getReturnTypeOfSignature (~37810)
     * applied to the VariableDeclaration initializer. Fixture:
     * checker/generic_constraints/05_two_params_one_constrained.ts. Verifies:
     *   - PropertySignature entries for `k` and `v` in the return TypeLiteral
     *   - variable `p`'s type has K substituted with the literal `"name"`
     *     (constraint-preserved) and V widened to `number` (unconstrained).
     */
    {
        const char* src =
            "// @checker: types\n"
            "function pair<K extends string, V>(k: K, v: V): { k: K; v: V } {\n"
            "  return { k, v };\n"
            "}\n"
            "const p = pair(\"name\", 42);\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        bool saw_ret_k = false;
        bool saw_ret_v = false;
        bool saw_p = false;
        for (size_t i = 0; i < cr->entries_len; ++i) {
            const CtscCheckTypeEntry* e = &cr->entries[i];
            if (!e->decl_kind_name) continue;
            if (strcmp(e->decl_kind_name, "PropertySignature") == 0
                && e->name_len == 1 && e->name[0] == 'k') {
                saw_ret_k = true;
            }
            if (strcmp(e->decl_kind_name, "PropertySignature") == 0
                && e->name_len == 1 && e->name[0] == 'v') {
                saw_ret_v = true;
            }
            if (strcmp(e->decl_kind_name, "VariableDeclaration") == 0
                && e->name_len == 1 && e->name[0] == 'p') {
                saw_p = true;
                CtscBuffer ts;
                ctsc_buf_init(&ts);
                ctsc_type_to_string(e->type, &ts);
                const char* want = "{ k: \"name\"; v: number; }";
                EXPECT(ts.len == strlen(want) && memcmp(ts.data, want, strlen(want)) == 0);
                ctsc_buf_free(&ts);
            }
        }
        EXPECT(saw_ret_k);
        EXPECT(saw_ret_v);
        EXPECT(saw_p);
        ctsc_arena_free(&a);
    }

    /*
     * TS2554 when a call has too few arguments (checker.ts getArgumentArityError
     * ~36458-36477, Diagnostics.Expected_0_arguments_but_got_1). Mirrors
     * fixtures/checker/function_calls/02_too_few_args.ts (CRLF).
     */
    {
        const char* src = "// @checker: diag\r\nfunction f(x: number) {}\r\nf();\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2554);
            EXPECT(cr->diagnostics[0].start == 45);
            EXPECT(cr->diagnostics[0].length == 1);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Expected 1 arguments, but got 0.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /*
     * TS2554 when a call passes more arguments than the callee accepts
     * (checker.ts getArgumentArityError ~36479-36493). Error span on excess
     * args. Mirrors fixtures/checker/function_calls/03_too_many_args.ts (CRLF).
     */
    {
        const char* src = "// @checker: diag\r\nfunction f() {}\r\nf(1);\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2554);
            EXPECT(cr->diagnostics[0].start == 38);
            EXPECT(cr->diagnostics[0].length == 1);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Expected 0 arguments, but got 1.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /* Annotated widen still assignable: no TS2322. */
    {
        const char* src = "const g: number = 42;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        ctsc_arena_free(&a);
    }

    /* `const n = null` types as any with strictNullChecks off (checker.ts nullWideningType + getWidenedType ~2093, ~26027). */
    {
        const char* src = "// @checker: types\nconst n = null;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 3 && memcmp(ts.data, "any", 3) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Unary minus on numeric literal: NumberLiteralType mirrors tsc (checker.ts checkPrefixUnaryExpression ~39993-39994). */
    {
        const char* src = "// @checker: types\nconst neg = -5;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 2 && memcmp(ts.data, "-5", 2) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* `as` / angle assertion: expression type is the asserted type (checkAssertionWorker ~38110-38123;
     * fixtures/checker/type_assertion/01_as_cast.ts). */
    {
        const char* src = "// @checker: types\nconst x = 1 as number;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Identifier initializer reads prior const binding type (getTypeOfSymbol ~12537; fixture checker/references/01). */
    {
        const char* src = "// @checker: types\nconst a = 1;\nconst b = a;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len == 2 && cr->entries[0].type && cr->entries[1].type) {
            CtscBuffer t0, t1;
            ctsc_buf_init(&t0);
            ctsc_buf_init(&t1);
            ctsc_type_to_string(cr->entries[0].type, &t0);
            ctsc_type_to_string(cr->entries[1].type, &t1);
            EXPECT(t0.len == 1 && memcmp(t0.data, "1", 1) == 0);
            EXPECT(t1.len == 1 && memcmp(t1.data, "1", 1) == 0);
            ctsc_buf_free(&t0);
            ctsc_buf_free(&t1);
        }
        ctsc_arena_free(&a);
    }

    /* `for (let i = 0; ...)` block-scoped loop var + body const (binder.ts bindForStatement ~1541, getContainerFlags ~3876). */
    {
        const char* src = "// @checker: types\nfor (let i = 0; i < 10; i = i + 1) {\n  const x = i;\n}\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len == 2 && cr->entries[0].type && cr->entries[1].type) {
            CtscBuffer t0, t1;
            ctsc_buf_init(&t0);
            ctsc_buf_init(&t1);
            ctsc_type_to_string(cr->entries[0].type, &t0);
            ctsc_type_to_string(cr->entries[1].type, &t1);
            EXPECT(t0.len == 6 && memcmp(t0.data, "number", 6) == 0);
            EXPECT(t1.len == 6 && memcmp(t1.data, "number", 6) == 0);
            ctsc_buf_free(&t0);
            ctsc_buf_free(&t1);
        }
        ctsc_arena_free(&a);
    }

    /* NoSubstitutionTemplateLiteral: string literal type uses cooked text (checker.ts ~32611, LiteralExpression). */
    {
        const char* src = "// @checker: types\nconst t = `hello`;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 7 && memcmp(ts.data, "\"hello\"", 7) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Annotated `number[] = []`: noLib array + postfix intrinsic → `{}` on the binding (checker.ts createArrayType ~33399). */
    {
        const char* src = "// @checker: types\nconst a: number[] = [];\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 2 && memcmp(ts.data, "{}", 2) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Array literal with harness noLib: typeToString is `{}` (checker.ts checkArrayLiteral ~33329). */
    {
        const char* src = "// @checker: types\nconst a = [1, 2, 3];\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 2 && memcmp(ts.data, "{}", 2) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Tuple annotation + array literal initializer (fixtures/checker/tuples/01_simple_tuple.ts;
     * checker.ts getTypeFromArrayOrTupleTypeNode ~17824-17840). */
    {
        const char* src = "// @checker: types\nconst t: [number, string] = [1, \"a\"];\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            const char* want = "[number, string]";
            EXPECT(ts.len == strlen(want) && memcmp(ts.data, want, ts.len) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Tuple element access with numeric literal index (checker.ts getPropertyTypeForIndexType ~19262-19279;
     * fixtures/checker/tuples/02_tuple_access.ts). */
    {
        const char* src = "// @checker: types\nconst t: [number, string] = [1, \"a\"];\nconst n = t[0];\nconst s = t[1];\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len == 3 && cr->entries[1].type && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Optional tuple element annotation `[number, string?]` (checker.ts
     * typeToTypeNodeHelper ~7432-7454, ElementFlags.Optional; fixtures/checker/
     * tuples_advanced/01_optional_tuple_element.ts). */
    {
        const char* src = "// @checker: types\ndeclare const t: [number, string?];\nconst a = t[0];\nconst b = t[1];\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len == 3 && cr->entries[0].type && cr->entries[1].type && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            const char* want0 = "[number, string?]";
            EXPECT(ts.len == strlen(want0) && memcmp(ts.data, want0, ts.len) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Rest tuple element annotation `[string, ...number[]]` (checker.ts
     * typeToTypeNodeHelper ~7447-7452, ElementFlags.Rest; fixtures/checker/
     * tuples_advanced/02_rest_tuple_element.ts). Index past the fixed head
     * resolves to the rest element's inner type (getPropertyTypeForIndexType
     * ~19262-19279 / getTupleElementTypeOutOfStartCount). */
    {
        const char* src = "// @checker: types\ndeclare const t: [string, ...number[]];\n"
                          "const a = t[0];\nconst b = t[1];\nconst c = t[2];\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 4);
        if (cr->entries_len == 4 && cr->entries[0].type && cr->entries[1].type && cr->entries[2].type
            && cr->entries[3].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            const char* want0 = "[string, ...number[]]";
            EXPECT(ts.len == strlen(want0) && memcmp(ts.data, want0, ts.len) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[3].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Readonly tuple annotation `readonly [number, string]` (checker.ts
     * getArrayOrTupleTargetType ~17745-17752 sets TupleType.readonly when the
     * TupleTypeNode's parent is a readonly TypeOperatorNode; typeToTypeNodeHelper
     * ~7457-7463 wraps the printed TupleTypeNode in TypeOperator(ReadonlyKeyword).
     * Element access still yields the element type unchanged (readonly is a
     * mutation-time guard, not a type substitution). fixtures/checker/
     * tuples_advanced/03_readonly_tuple.ts. */
    {
        const char* src = "// @checker: types\ndeclare const t: readonly [number, string];\n"
                          "const a = t[0];\nconst b = t[1];\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len == 3 && cr->entries[0].type && cr->entries[1].type && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            const char* want0 = "readonly [number, string]";
            EXPECT(ts.len == strlen(want0) && memcmp(ts.data, want0, ts.len) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* NamedTupleMember annotation `[id: number, name: string]` (parser.ts
     * parseTupleElementNameOrTupleElementType ~4464-4477; checker.ts
     * typeToTypeNodeHelper ~7437-7449 re-emits labeled members via
     * factory.createNamedTupleMember). Element access by numeric literal
     * still yields the element's type (labels are purely for display;
     * checker.ts getPropertyTypeForIndexType ~19262-19279 ignores labels).
     * fixtures/checker/tuples_advanced/04_named_tuple.ts. */
    {
        const char* src = "// @checker: types\ndeclare const t: [id: number, name: string];\n"
                          "const a = t[0];\nconst b = t[1];\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len == 3 && cr->entries[0].type && cr->entries[1].type && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            const char* want0 = "[id: number, name: string]";
            EXPECT(ts.len == strlen(want0) && memcmp(ts.data, want0, ts.len) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Simple object literal: checkObjectLiteral + widened property types (checker.ts ~33527, ~41503). */
    {
        const char* src = "// @checker: types\nconst o = { a: 1 };\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            const char* want = "{ a: number; }";
            EXPECT(ts.len == strlen(want) && memcmp(ts.data, want, ts.len) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Property read: getTypeOfPropertyOfType (checker.ts ~11575); fixtures/checker/property_access/01_read_property.ts. */
    {
        const char* src = "// @checker: types\nconst o = { a: 1 };\nconst x = o.a;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len == 2 && cr->entries[1].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Inherited class field: `extends` heritage + getTypeOfPropertyOfType (~11575);
     * fixtures/checker/class_inheritance/02_inherit_field.ts. */
    {
        const char* src =
            "// @checker: types\n"
            "class A {\n"
            "  x: number = 1;\n"
            "}\n"
            "class B extends A {}\n"
            "const b = new B();\n"
            "const n = b.x;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len == 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Interface `extends`: structural members for property access (checker.ts
     * resolveBaseTypesOfInterface ~13400; fixtures/checker/interface_extends/01_simple.ts). */
    {
        const char* src =
            "// @checker: types\n"
            "interface A {\n"
            "  x: number;\n"
            "}\n"
            "interface B extends A {}\n"
            "const b: B = { x: 1 };\n"
            "const n = b.x;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len == 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Missing property on object literal: TS2339 (checker.ts ~34948-34968;
     * fixtures/checker/property_access/02_missing_property.ts). */
    {
        const char* src = "// @checker: diag\r\nconst o = { a: 1 };\r\no.b;\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2339);
            EXPECT(cr->diagnostics[0].start == 42);
            EXPECT(cr->diagnostics[0].length == 1);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Property 'b' does not exist on type '{ a: number; }'.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /* Binary `+` on numbers → `number` (checker.ts checkBinaryLikeExpressionWorker ~40837-40840). */
    {
        const char* src = "// @checker: types\nconst s = 1 + 2;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* String concatenation widens to `string` for const (same PlusToken branch ~40846-40848). */
    {
        const char* src = "// @checker: types\nconst s = \"a\" + \"b\";\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Annotated primitive union: getTypeFromTypeNode + union ordering (checker.ts ~15000, ~14780). */
    {
        const char* src = "// @checker: types\nlet x: number | string = 1;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 15 && memcmp(ts.data, "string | number", 15) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* ConditionalExpression: union of branch types (checker.ts checkConditionalExpression ~41268-41273). */
    {
        const char* src = "// @checker: types\nconst x = true ? 1 : 2;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 5 && memcmp(ts.data, "1 | 2", 5) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* BigInt literal type string matches checker.ts typeToString on BigIntLiteralType (~6860). */
    {
        const char* src = "const bi = 42n;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 3 && memcmp(ts.data, "42n", 3) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Inferred function return type: getReturnTypeFromBody (~39195) + getWidenedType (~39277). */
    {
        const char* src = "// @checker: types\nfunction f() {\n  return 42;\n}\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            EXPECT(cr->entries[0].type_string_len == 12);
            EXPECT(memcmp(cr->entries[0].type_string, "() => number", 12) == 0);
        }
        ctsc_arena_free(&a);
    }

    /* FunctionExpression with no params (fixtures/checker/function_expressions/01_no_params.ts;
     * checker.ts getReturnTypeFromBody ~39195-39250 + getWidenedType ~39276-39277). */
    {
        const char* src = "// @checker: types\nconst f = function () {\n  return 1;\n};\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            EXPECT(cr->entries[0].type_string_len == 12);
            EXPECT(memcmp(cr->entries[0].type_string, "() => number", 12) == 0);
        }
        ctsc_arena_free(&a);
    }

    /* Multiple returns with distinct literal types (fixtures/checker/multi_return/01_same_type.ts;
     * checker.ts getReturnTypeFromBody ~39249-39250 + getWidenedType ~39276-39277). */
    {
        const char* src =
            "// @checker: types\n"
            "function f(b: boolean) {\n"
            "  if (b) {\n"
            "    return 1;\n"
            "  }\n"
            "  return 2;\n"
            "}\n"
            "const n = f(true);\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len >= 1 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            const char* want = "(b: boolean) => 1 | 2";
            EXPECT(cr->entries[0].type_string_len == strlen(want));
            EXPECT(memcmp(cr->entries[0].type_string, want, strlen(want)) == 0);
        }
        if (cr->entries_len >= 3 && cr->entries[2].type_string && cr->entries[2].type_string_len > 0) {
            const char* want_n = "1 | 2";
            EXPECT(cr->entries[2].type_string_len == strlen(want_n));
            EXPECT(memcmp(cr->entries[2].type_string, want_n, strlen(want_n)) == 0);
        }
        ctsc_arena_free(&a);
    }

    /*
     * typeof narrowing + union param in signature string (fixtures/checker/narrowing/01_typeof_string.ts;
     * checker.ts narrowTypeByTypeFacts / getNarrowedType ~38000+, typeToString on signatures ~6202).
     */
    {
        const char* src = "// @checker: types\n"
                          "function f(x: string | number) {\n"
                          "  if (typeof x === \"string\") {\n"
                          "    const s = x;\n"
                          "  } else {\n"
                          "    const n = x;\n"
                          "  }\n"
                          "}\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 4);
        if (cr->entries_len >= 1 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            const char* want_f = "(x: string | number) => void";
            EXPECT(cr->entries[0].type_string_len == strlen(want_f));
            EXPECT(memcmp(cr->entries[0].type_string, want_f, strlen(want_f)) == 0);
        }
        if (cr->entries_len >= 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
        }
        if (cr->entries_len >= 4 && cr->entries[3].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[3].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /*
     * typeof === "number" narrowing (fixtures/checker/narrowing/02_typeof_number.ts;
     * checker.ts narrowTypeByTypeFacts / TypeofEQNumber ~29867).
     */
    {
        const char* src = "// @checker: types\n"
                          "function f(x: string | number) {\n"
                          "  if (typeof x === \"number\") {\n"
                          "    const n = x;\n"
                          "  }\n"
                          "}\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len >= 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /*
     * typeof === "boolean" narrowing (fixtures/checker/narrowing/03_typeof_boolean.ts;
     * checker.ts narrowTypeByTypeFacts / TypeofEQBoolean ~29871).
     */
    {
        const char* src = "// @checker: types\n"
                          "function f(x: string | boolean) {\n"
                          "  if (typeof x === \"boolean\") {\n"
                          "    const b = x;\n"
                          "  }\n"
                          "}\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len >= 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 7 && memcmp(ts.data, "boolean", 7) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /*
     * Harness default strictNullChecks off widens unions for getTypeAtLocation dumps
     * (fixtures/checker/narrowing/04_null_check.ts; checker.ts getWidenedType ~26021+).
     */
    {
        const char* src = "// @checker: types\n"
                          "function f(x: string | null) {\n"
                          "  if (x !== null) {\n"
                          "    const s = x;\n"
                          "  }\n"
                          "}\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len >= 2 && cr->entries[1].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
        }
        if (cr->entries_len >= 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "string", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /*
     * Union of string literal types + `===` discriminant narrowing (fixtures/checker/narrowing/
     * 07_discriminant_literal.ts; checker.ts narrowTypeByEquality ~29752-29787).
     */
    {
        const char* src = "// @checker: types\n"
                          "function f(tag: \"a\" | \"b\") {\n"
                          "  if (tag === \"a\") {\n"
                          "    const x = tag;\n"
                          "  }\n"
                          "}\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len >= 2 && cr->entries[1].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            const char* want_tag = "\"a\" | \"b\"";
            EXPECT(ts.len == strlen(want_tag) && memcmp(ts.data, want_tag, strlen(want_tag)) == 0);
            ctsc_buf_free(&ts);
        }
        if (cr->entries_len >= 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            const char* want_x = "\"a\"";
            EXPECT(ts.len == strlen(want_x) && memcmp(ts.data, want_x, strlen(want_x)) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* FunctionExpression with one typed parameter (fixtures/checker/function_expressions/02_one_param.ts). */
    {
        const char* src = "// @checker: types\nconst f = function (x: number) {\n  return x;\n};\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len >= 2 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            EXPECT(cr->entries[0].type_string_len == 21);
            EXPECT(memcmp(cr->entries[0].type_string, "(x: number) => number", 21) == 0);
        }
        if (cr->entries_len >= 2 && cr->entries[1].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Arrow function with no params (fixtures/checker/arrow_functions/01_no_params.ts;
     * checker.ts getReturnTypeFromBody ~39208-39210). */
    {
        const char* src = "// @checker: types\nconst f = () => 1;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            EXPECT(cr->entries[0].type_string_len == 12);
            EXPECT(memcmp(cr->entries[0].type_string, "() => number", 12) == 0);
        }
        ctsc_arena_free(&a);
    }

    /* One typed parameter + identity body (fixtures/checker/arrow_functions/02_one_param.ts;
     * checkExpressionCached on concise body ~39208-39210). */
    {
        const char* src = "// @checker: types\nconst f = (x: number) => x;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len >= 2 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            EXPECT(cr->entries[0].type_string_len == 21);
            EXPECT(memcmp(cr->entries[0].type_string, "(x: number) => number", 21) == 0);
        }
        if (cr->entries_len >= 2 && cr->entries[1].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Generic identity + inferred literal (fixtures/checker/generics/01_identity_number.ts;
     * checker.ts signatureToString ~6202, inferTypeArguments / getReturnTypeOfSignature ~37810+). */
    {
        const char* src = "// @checker: types\n"
                          "function id<T>(x: T): T {\n"
                          "  return x;\n"
                          "}\n"
                          "const n = id(42);\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len >= 1 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            EXPECT(cr->entries[0].type_string_len == 14);
            EXPECT(memcmp(cr->entries[0].type_string, "<T>(x: T) => T", 14) == 0);
        }
        if (cr->entries_len >= 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 2 && memcmp(ts.data, "42", 2) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Explicit type argument widens argument-side inference (fixtures/checker/generics/03_identity_explicit_type_arg.ts;
     * checker.ts inferTypeArguments when typeArguments are present ~35827+). */
    {
        const char* src = "// @checker: types\n"
                          "function id<T>(x: T): T {\n"
                          "  return x;\n"
                          "}\n"
                          "const n = id<number>(42);\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len >= 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Two type parameters: return type A inferred from first arg (fixtures/checker/generics/04_two_type_params.ts;
     * checker.ts inferTypeArguments / getReturnTypeOfSignature ~35827+, ~37810+). */
    {
        const char* src = "// @checker: types\n"
                          "function first<A, B>(a: A, b: B): A {\n"
                          "  return a;\n"
                          "}\n"
                          "const n = first(1, \"x\");\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 4);
        if (cr->entries_len >= 4 && cr->entries[3].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[3].type, &ts);
            EXPECT(ts.len == 1 && memcmp(ts.data, "1", 1) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Annotated object type (fixtures/checker/objects/03_annotated_object.ts). */
    {
        const char* src = "// @checker: types\r\nconst o: { a: number } = { a: 1 };\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        CtscNode* stm0 = pr.sourceFile->data.sourceFile.statements.items[0];
        EXPECT(stm0 && stm0->kind == CTSC_SK_VariableStatement);
        CtscNode* vdl = stm0->data.variableStatement.declarationList;
        CtscNode* vd0 = vdl->data.variableDeclarationList.declarations.items[0];
        EXPECT(vd0 && vd0->kind == CTSC_SK_VariableDeclaration);
        EXPECT(vd0->data.variableDeclaration.type
               && vd0->data.variableDeclaration.type->kind == CTSC_SK_TypeLiteral);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len >= 2 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 14 && memcmp(ts.data, "{ a: number; }", 14) == 0);
            ctsc_buf_free(&ts);
        }
        if (cr->entries_len >= 2) {
            EXPECT(cr->entries[1].type != NULL);
            if (cr->entries[1].type) {
                CtscBuffer t2;
                ctsc_buf_init(&t2);
                ctsc_type_to_string(cr->entries[1].type, &t2);
                EXPECT(t2.len == 6 && memcmp(t2.data, "number", 6) == 0);
                ctsc_buf_free(&t2);
            }
            /* Matches tsc Identifier pos/end (token full start includes trivia before `a`). */
            EXPECT(cr->entries[1].pos == 30 && cr->entries[1].end == 32);
        }
        ctsc_arena_free(&a);
    }

    /* Simple interface (fixtures/checker/interfaces/01_simple_interface.ts; checker.ts getTypeFromTypeNode ~15000). */
    {
        const char* src = "// @checker: types\ninterface P {\n  a: number;\n}\nconst p: P = { a: 1 };\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len >= 2) {
            /* Visit order: interface members then variable (JSON dump sorts by pos). */
            const CtscCheckTypeEntry* ps = NULL;
            const CtscCheckTypeEntry* vd = NULL;
            for (size_t i = 0; i < cr->entries_len; i++) {
                if (cr->entries[i].decl_kind_name
                    && strcmp(cr->entries[i].decl_kind_name, "PropertySignature") == 0)
                    ps = &cr->entries[i];
                if (cr->entries[i].decl_kind_name
                    && strcmp(cr->entries[i].decl_kind_name, "VariableDeclaration") == 0)
                    vd = &cr->entries[i];
            }
            EXPECT(ps != NULL && vd != NULL);
            if (ps && ps->type) {
                CtscBuffer t0;
                ctsc_buf_init(&t0);
                ctsc_type_to_string(ps->type, &t0);
                EXPECT(t0.len == 6 && memcmp(t0.data, "number", 6) == 0);
                ctsc_buf_free(&t0);
            }
            if (vd && vd->type) {
                CtscBuffer t1;
                ctsc_buf_init(&t1);
                ctsc_type_to_string(vd->type, &t1);
                EXPECT(t1.len == 1 && memcmp(t1.data, "P", 1) == 0);
                ctsc_buf_free(&t1);
            }
        }
        ctsc_arena_free(&a);
    }

    /* Intersection of two interfaces (fixtures/checker/intersection/01_two_types.ts;
     * checker.ts getTypeFromIntersectionTypeNode ~14510+). */
    {
        const char* src = "// @checker: types\n"
                          "interface A {\n"
                          "  x: number;\n"
                          "}\n"
                          "interface B {\n"
                          "  y: string;\n"
                          "}\n"
                          "const ab: A & B = { x: 1, y: \"hi\" };\n"
                          "const n = ab.x;\n"
                          "const s = ab.y;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 5);
        const CtscCheckTypeEntry* e_ab = NULL;
        const CtscCheckTypeEntry* e_n = NULL;
        const CtscCheckTypeEntry* e_s = NULL;
        for (size_t i = 0; i < cr->entries_len; i++) {
            if (cr->entries[i].name_len == 2 && cr->entries[i].name
                && cr->entries[i].name[0] == (uint16_t)'a' && cr->entries[i].name[1] == (uint16_t)'b')
                e_ab = &cr->entries[i];
            if (cr->entries[i].name_len == 1 && cr->entries[i].name && cr->entries[i].name[0] == (uint16_t)'n')
                e_n = &cr->entries[i];
            if (cr->entries[i].name_len == 1 && cr->entries[i].name && cr->entries[i].name[0] == (uint16_t)'s')
                e_s = &cr->entries[i];
        }
        EXPECT(e_ab != NULL && e_n != NULL && e_s != NULL);
        if (e_ab && e_ab->type) {
            CtscBuffer t;
            ctsc_buf_init(&t);
            ctsc_type_to_string(e_ab->type, &t);
            EXPECT(t.len == 5 && memcmp(t.data, "A & B", 5) == 0);
            ctsc_buf_free(&t);
        }
        if (e_n && e_n->type) {
            CtscBuffer t;
            ctsc_buf_init(&t);
            ctsc_type_to_string(e_n->type, &t);
            EXPECT(t.len == 6 && memcmp(t.data, "number", 6) == 0);
            ctsc_buf_free(&t);
        }
        if (e_s && e_s->type) {
            CtscBuffer t;
            ctsc_buf_init(&t);
            ctsc_type_to_string(e_s->type, &t);
            EXPECT(t.len == 6 && memcmp(t.data, "string", 6) == 0);
            ctsc_buf_free(&t);
        }
        ctsc_arena_free(&a);
    }

    /* String index signature: Parameter entry + element access type (fixtures/checker/index_signature/01_string_index.ts;
     * checker.ts getPropertyTypeForIndexType ~19262-19279). */
    {
        const char* src = "// @checker: types\r\n"
                          "interface Dict {\r\n"
                          "  [key: string]: number;\r\n"
                          "}\r\n"
                          "const d: Dict = { a: 1, b: 2 };\r\n"
                          "const n = d[\"a\"];\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        const CtscCheckTypeEntry* param = NULL;
        const CtscCheckTypeEntry* vd_d = NULL;
        const CtscCheckTypeEntry* vd_n = NULL;
        for (size_t i = 0; i < cr->entries_len; i++) {
            if (cr->entries[i].decl_kind_name && strcmp(cr->entries[i].decl_kind_name, "Parameter") == 0)
                param = &cr->entries[i];
            if (cr->entries[i].decl_kind_name && strcmp(cr->entries[i].decl_kind_name, "VariableDeclaration") == 0
                && cr->entries[i].name_len == 1 && cr->entries[i].name && cr->entries[i].name[0] == (uint16_t)'d')
                vd_d = &cr->entries[i];
            if (cr->entries[i].decl_kind_name && strcmp(cr->entries[i].decl_kind_name, "VariableDeclaration") == 0
                && cr->entries[i].name_len == 1 && cr->entries[i].name && cr->entries[i].name[0] == (uint16_t)'n')
                vd_n = &cr->entries[i];
        }
        EXPECT(param != NULL && vd_d != NULL && vd_n != NULL);
        if (param && param->type) {
            CtscBuffer t0;
            ctsc_buf_init(&t0);
            ctsc_type_to_string(param->type, &t0);
            EXPECT(t0.len == 6 && memcmp(t0.data, "string", 6) == 0);
            ctsc_buf_free(&t0);
        }
        if (vd_n && vd_n->type) {
            CtscBuffer t1;
            ctsc_buf_init(&t1);
            ctsc_type_to_string(vd_n->type, &t1);
            EXPECT(t1.len == 6 && memcmp(t1.data, "number", 6) == 0);
            ctsc_buf_free(&t1);
        }
        ctsc_arena_free(&a);
    }

    /* Optional PropertySignature (fixtures/checker/optional/01_optional_field.ts;
     * parser.ts parsePropertyOrMethodSignature ~4268–4282). */
    {
        const char* src = "// @checker: types\r\n"
                          "interface P {\r\n"
                          "  x?: number;\r\n"
                          "}\r\n"
                          "const p: P = {};\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        const CtscCheckTypeEntry* ps = NULL;
        const CtscCheckTypeEntry* vd = NULL;
        for (size_t i = 0; i < cr->entries_len; i++) {
            if (cr->entries[i].decl_kind_name
                && strcmp(cr->entries[i].decl_kind_name, "PropertySignature") == 0)
                ps = &cr->entries[i];
            if (cr->entries[i].decl_kind_name
                && strcmp(cr->entries[i].decl_kind_name, "VariableDeclaration") == 0)
                vd = &cr->entries[i];
        }
        EXPECT(ps != NULL && vd != NULL);
        if (ps && ps->type) {
            CtscBuffer t0;
            ctsc_buf_init(&t0);
            ctsc_type_to_string(ps->type, &t0);
            EXPECT(t0.len == 6 && memcmp(t0.data, "number", 6) == 0);
            ctsc_buf_free(&t0);
        }
        if (ps) EXPECT(ps->pos == 33 && ps->end == 38);
        if (vd) EXPECT(vd->pos == 58 && vd->end == 60);
        ctsc_arena_free(&a);
    }

    /* Readonly PropertySignature + property access type (fixtures/checker/readonly/
     * 01_readonly_field.ts; parser.ts parsePropertySignature ~4435, checker.ts
     * getTypeOfPropertyOfType ~11575). */
    {
        const char* src = "// @checker: types\r\n"
                          "interface P {\r\n"
                          "  readonly x: number;\r\n"
                          "}\r\n"
                          "const p: P = { x: 1 };\r\n"
                          "const n = p.x;\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        const CtscCheckTypeEntry* ps = NULL;
        const CtscCheckTypeEntry* vd_p = NULL;
        const CtscCheckTypeEntry* vd_n = NULL;
        for (size_t i = 0; i < cr->entries_len; i++) {
            if (cr->entries[i].decl_kind_name
                && strcmp(cr->entries[i].decl_kind_name, "PropertySignature") == 0)
                ps = &cr->entries[i];
            if (cr->entries[i].decl_kind_name
                && strcmp(cr->entries[i].decl_kind_name, "VariableDeclaration") == 0
                && cr->entries[i].name_len == 1 && cr->entries[i].name
                && cr->entries[i].name[0] == (uint16_t)'p')
                vd_p = &cr->entries[i];
            if (cr->entries[i].decl_kind_name
                && strcmp(cr->entries[i].decl_kind_name, "VariableDeclaration") == 0
                && cr->entries[i].name_len == 1 && cr->entries[i].name
                && cr->entries[i].name[0] == (uint16_t)'n')
                vd_n = &cr->entries[i];
        }
        EXPECT(ps != NULL && vd_p != NULL && vd_n != NULL);
        if (ps) EXPECT(ps->pos == 45 && ps->end == 47);
        if (vd_p) EXPECT(vd_p->pos == 66 && vd_p->end == 68);
        if (vd_n) EXPECT(vd_n->pos == 90 && vd_n->end == 92);
        if (ps && ps->type) {
            CtscBuffer t0;
            ctsc_buf_init(&t0);
            ctsc_type_to_string(ps->type, &t0);
            EXPECT(t0.len == 6 && memcmp(t0.data, "number", 6) == 0);
            ctsc_buf_free(&t0);
        }
        if (vd_n && vd_n->type) {
            CtscBuffer t1;
            ctsc_buf_init(&t1);
            ctsc_type_to_string(vd_n->type, &t1);
            EXPECT(t1.len == 6 && memcmp(t1.data, "number", 6) == 0);
            ctsc_buf_free(&t1);
        }
        ctsc_arena_free(&a);
    }

    /* Optional parameter on FunctionDeclaration (fixtures/checker/optional/02_optional_param.ts;
     * parser.ts parseParameterWorker ~4081, checker.ts signatureToString / typeToString ~6202). */
    {
        const char* src = "// @checker: types\nfunction f(x?: number): number {\n  return 1;\n}\nconst n = f();\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len >= 1 && cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
            const char* want = "(x?: number) => number";
            EXPECT(cr->entries[0].type_string_len == strlen(want));
            EXPECT(memcmp(cr->entries[0].type_string, want, strlen(want)) == 0);
        }
        ctsc_arena_free(&a);
    }

    /* Two-field interface + const: second PropertySignature name pos includes trivia
     * after `;` (fixtures/checker/interfaces/02_interface_two_fields.ts, CRLF; tsc
     * Identifier fullStart; checker.ts createIdentifier ~2648). */
    {
        const char* src = "// @checker: types\r\n"
                          "interface P {\r\n"
                          "  a: number;\r\n"
                          "  b: string;\r\n"
                          "}\r\n"
                          "const p: P = { a: 1, b: \"x\" };\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(len == 98);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        const CtscCheckTypeEntry* ea = NULL;
        const CtscCheckTypeEntry* eb = NULL;
        const CtscCheckTypeEntry* ep = NULL;
        for (size_t i = 0; i < cr->entries_len; i++) {
            const CtscCheckTypeEntry* e = &cr->entries[i];
            if (!e->decl_kind_name || !e->name || e->name_len != 1u) continue;
            char ch = (char)e->name[0];
            if (strcmp(e->decl_kind_name, "PropertySignature") == 0 && ch == 'a') ea = e;
            if (strcmp(e->decl_kind_name, "PropertySignature") == 0 && ch == 'b') eb = e;
            if (strcmp(e->decl_kind_name, "VariableDeclaration") == 0 && ch == 'p') ep = e;
        }
        EXPECT(ea != NULL && eb != NULL && ep != NULL);
        if (ea) EXPECT(ea->pos == 33 && ea->end == 38);
        if (eb) EXPECT(eb->pos == 47 && eb->end == 52);
        if (ep) EXPECT(ep->pos == 71 && ep->end == 73);
        ctsc_arena_free(&a);
    }

    /* Type alias: annotated variable type string is the alias target, not the alias name
     * (fixtures/checker/type_aliases/01_alias_number.ts; checker.ts getDeclaredTypeOfTypeAlias ~9900). */
    {
        const char* src = "// @checker: types\ntype N = number;\nconst x: N = 1;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Object type alias: PropertySignature entries from type literal (fixtures/checker/
     * type_aliases/03_alias_object.ts; same shape as InterfaceDeclaration members).
     * Source uses CRLF like the on-disk fixture (UTF-16 positions match tsc). */
    {
        const char* src = "// @checker: types\r\ntype P = { a: number };\r\nconst p: P = { a: 1 };\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len >= 2) {
            const CtscCheckTypeEntry* ps = NULL;
            const CtscCheckTypeEntry* vd = NULL;
            for (size_t i = 0; i < cr->entries_len; i++) {
                if (cr->entries[i].decl_kind_name
                    && strcmp(cr->entries[i].decl_kind_name, "PropertySignature") == 0)
                    ps = &cr->entries[i];
                if (cr->entries[i].decl_kind_name
                    && strcmp(cr->entries[i].decl_kind_name, "VariableDeclaration") == 0)
                    vd = &cr->entries[i];
            }
            EXPECT(ps != NULL && vd != NULL);
            if (ps && ps->type) {
                CtscBuffer t0;
                ctsc_buf_init(&t0);
                ctsc_type_to_string(ps->type, &t0);
                EXPECT(t0.len == 6 && memcmp(t0.data, "number", 6) == 0);
                ctsc_buf_free(&t0);
            }
            if (vd && vd->type) {
                CtscBuffer t1;
                ctsc_buf_init(&t1);
                ctsc_type_to_string(vd->type, &t1);
                EXPECT(t1.len == 1 && memcmp(t1.data, "P", 1) == 0);
                ctsc_buf_free(&t1);
            }
            if (ps) EXPECT(ps->pos == 30 && ps->end == 32);
            if (vd) EXPECT(vd->pos == 50 && vd->end == 52);
        }
        ctsc_arena_free(&a);
    }

    /* Union type alias: typeToString uses alias name (fixtures/checker/type_aliases/
     * 02_alias_union.ts; checker.ts typeToTypeNodeWorker ~6916). */
    {
        const char* src = "// @checker: types\ntype SN = string | number;\nlet x: SN = 1;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 2 && memcmp(ts.data, "SN", 2) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Class field + property access (fixtures/checker/classes/02_class_with_field.ts;
     * checker.ts getTypeOfPropertyOfType ~11575). */
    {
        const char* src = "// @checker: types\nclass C {\n  x: number = 1;\n}\nconst c = new C();\nconst n = c.x;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 3);
        if (cr->entries_len == 3) {
            EXPECT(cr->entries[0].decl_kind_name
                   && strcmp(cr->entries[0].decl_kind_name, "PropertyDeclaration") == 0);
            EXPECT(cr->entries[1].decl_kind_name
                   && strcmp(cr->entries[1].decl_kind_name, "VariableDeclaration") == 0);
            EXPECT(cr->entries[2].decl_kind_name
                   && strcmp(cr->entries[2].decl_kind_name, "VariableDeclaration") == 0);
            if (cr->entries[0].type) {
                CtscBuffer t0;
                ctsc_buf_init(&t0);
                ctsc_type_to_string(cr->entries[0].type, &t0);
                EXPECT(t0.len == 6 && memcmp(t0.data, "number", 6) == 0);
                ctsc_buf_free(&t0);
            }
            if (cr->entries[1].type) {
                CtscBuffer t1;
                ctsc_buf_init(&t1);
                ctsc_type_to_string(cr->entries[1].type, &t1);
                EXPECT(t1.len == 1 && memcmp(t1.data, "C", 1) == 0);
                ctsc_buf_free(&t1);
            }
            if (cr->entries[2].type) {
                CtscBuffer t2;
                ctsc_buf_init(&t2);
                ctsc_type_to_string(cr->entries[2].type, &t2);
                EXPECT(t2.len == 6 && memcmp(t2.data, "number", 6) == 0);
                ctsc_buf_free(&t2);
            }
        }
        ctsc_arena_free(&a);
    }

    /* Class getter return type on instance access (fixtures/checker/accessors/01_getter.ts;
     * checker.ts getTypeOfAccessors ~12706-12723, getTypeOfPropertyOfType ~11575). */
    {
        const char* src = "// @checker: types\nclass C {\n  _x: number = 1;\n  get x(): number {\n    return this._x;\n  }\n}\nconst n = new C().x;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len == 2) {
            EXPECT(cr->entries[0].decl_kind_name
                   && strcmp(cr->entries[0].decl_kind_name, "PropertyDeclaration") == 0);
            EXPECT(cr->entries[1].decl_kind_name
                   && strcmp(cr->entries[1].decl_kind_name, "VariableDeclaration") == 0);
            if (cr->entries[1].type) {
                CtscBuffer ts;
                ctsc_buf_init(&ts);
                ctsc_type_to_string(cr->entries[1].type, &ts);
                EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
                ctsc_buf_free(&ts);
            }
        }
        ctsc_arena_free(&a);
    }

    /* Class getter + setter: types channel includes setter parameter (fixtures/checker/
     * accessors/02_getter_setter.ts; checker.ts getTypeOfVariableOrParameterOrPropertyWorker
     * ~12643-12650). */
    {
        /* CRLF line endings match fixtures/checker/accessors/02_getter_setter.ts (181 bytes). */
        const char* src = "// @checker: types\r\n"
                          "class C {\r\n"
                          "  _x: number = 1;\r\n"
                          "  get x(): number {\r\n"
                          "    return this._x;\r\n"
                          "  }\r\n"
                          "  set x(v: number) {\r\n"
                          "    this._x = v;\r\n"
                          "  }\r\n"
                          "}\r\n"
                          "const c = new C();\r\n"
                          "const n = c.x;\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 4);
        if (cr->entries_len == 4) {
            EXPECT(cr->entries[0].decl_kind_name
                   && strcmp(cr->entries[0].decl_kind_name, "PropertyDeclaration") == 0);
            EXPECT(cr->entries[1].decl_kind_name
                   && strcmp(cr->entries[1].decl_kind_name, "Parameter") == 0);
            EXPECT(cr->entries[1].name_len == 1 && cr->entries[1].name
                   && cr->entries[1].name[0] == (uint16_t)'v');
            EXPECT(cr->entries[1].pos == 105 && cr->entries[1].end == 106);
            EXPECT(cr->entries[2].decl_kind_name
                   && strcmp(cr->entries[2].decl_kind_name, "VariableDeclaration") == 0);
            EXPECT(cr->entries[3].decl_kind_name
                   && strcmp(cr->entries[3].decl_kind_name, "VariableDeclaration") == 0);
            if (cr->entries[1].type) {
                CtscBuffer t1;
                ctsc_buf_init(&t1);
                ctsc_type_to_string(cr->entries[1].type, &t1);
                EXPECT(t1.len == 6 && memcmp(t1.data, "number", 6) == 0);
                ctsc_buf_free(&t1);
            }
            if (cr->entries[2].type) {
                CtscBuffer t2;
                ctsc_buf_init(&t2);
                ctsc_type_to_string(cr->entries[2].type, &t2);
                EXPECT(t2.len == 1 && memcmp(t2.data, "C", 1) == 0);
                ctsc_buf_free(&t2);
            }
            if (cr->entries[3].type) {
                CtscBuffer t3;
                ctsc_buf_init(&t3);
                ctsc_type_to_string(cr->entries[3].type, &t3);
                EXPECT(t3.len == 6 && memcmp(t3.data, "number", 6) == 0);
                ctsc_buf_free(&t3);
            }
        }
        ctsc_arena_free(&a);
    }

    /* Static field + access on class identifier (fixtures/checker/static/01_static_field.ts;
     * checker.ts getTypeOfSymbol staticType / getPropertyOfType ~10308, ~15893). */
    {
        const char* src =
            "// @checker: types\nclass C {\n  static x: number = 1;\n}\nconst n = C.x;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len == 2) {
            EXPECT(cr->entries[0].decl_kind_name
                   && strcmp(cr->entries[0].decl_kind_name, "PropertyDeclaration") == 0);
            EXPECT(cr->entries[1].decl_kind_name
                   && strcmp(cr->entries[1].decl_kind_name, "VariableDeclaration") == 0);
            if (cr->entries[0].type) {
                CtscBuffer t0;
                ctsc_buf_init(&t0);
                ctsc_type_to_string(cr->entries[0].type, &t0);
                EXPECT(t0.len == 6 && memcmp(t0.data, "number", 6) == 0);
                ctsc_buf_free(&t0);
            }
            if (cr->entries[1].type) {
                CtscBuffer t1;
                ctsc_buf_init(&t1);
                ctsc_type_to_string(cr->entries[1].type, &t1);
                EXPECT(t1.len == 6 && memcmp(t1.data, "number", 6) == 0);
                ctsc_buf_free(&t1);
            }
        }
        ctsc_arena_free(&a);
    }

    /* Empty class: `new C()` instance type is `C` (checker.ts resolveNewExpression ~37131, checkCallExpression ~37826). */
    {
        const char* src = "// @checker: types\nclass C {}\nconst c = new C();\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 8192);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 1 && memcmp(ts.data, "C", 1) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Class method (fixtures/checker/classes/03_class_with_method.ts; CRLF matches on-disk;
     * checker.ts getTypeOfSymbol / typeToString for method ~12537). */
    {
        const char* src = "// @checker: types\r\nclass C {\r\n  greet(): string {\r\n    return \"hi\";\r\n  }\r\n}\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len == 1) {
            EXPECT(cr->entries[0].decl_kind_name
                   && strcmp(cr->entries[0].decl_kind_name, "MethodDeclaration") == 0);
            EXPECT(cr->entries[0].pos == 29 && cr->entries[0].end == 38);
            if (cr->entries[0].type_string && cr->entries[0].type_string_len > 0) {
                EXPECT(cr->entries[0].type_string_len == 12);
                EXPECT(memcmp(cr->entries[0].type_string, "() => string", 12) == 0);
            }
        }
        ctsc_arena_free(&a);
    }

    /* Call instance method: checkCallExpression return type (fixtures/checker/class_methods/
     * 01_call_method.ts; checker.ts checkCallExpression ~37810-37878). */
    {
        const char* src = "// @checker: types\nclass C {\n  m(): number {\n    return 1;\n  }\n}\nconst n = new C().m();\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len >= 2 && cr->entries[1].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[1].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "number", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* Class method parameters in types JSON (fixtures/checker/class_methods/
     * 02_method_with_param.ts; checker.ts getTypeOfVariableOrParameterOrPropertyWorker ~12554). */
    {
        const char* src =
            "// @checker: types\nclass C {\n  add(x: number, y: number): number {\n    return x + y;\n  "
            "}\n}\nconst n = new C().add(1, 2);\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 4);
        if (cr->entries_len >= 4) {
            EXPECT(cr->entries[0].decl_kind_name
                   && strcmp(cr->entries[0].decl_kind_name, "MethodDeclaration") == 0);
            EXPECT(cr->entries[1].decl_kind_name && strcmp(cr->entries[1].decl_kind_name, "Parameter") == 0);
            EXPECT(cr->entries[2].decl_kind_name && strcmp(cr->entries[2].decl_kind_name, "Parameter") == 0);
            EXPECT(cr->entries[3].decl_kind_name
                   && strcmp(cr->entries[3].decl_kind_name, "VariableDeclaration") == 0);
            if (cr->entries[1].type) {
                CtscBuffer t1;
                ctsc_buf_init(&t1);
                ctsc_type_to_string(cr->entries[1].type, &t1);
                EXPECT(t1.len == 6 && memcmp(t1.data, "number", 6) == 0);
                ctsc_buf_free(&t1);
            }
            if (cr->entries[2].type) {
                CtscBuffer t2;
                ctsc_buf_init(&t2);
                ctsc_type_to_string(cr->entries[2].type, &t2);
                EXPECT(t2.len == 6 && memcmp(t2.data, "number", 6) == 0);
                ctsc_buf_free(&t2);
            }
            if (cr->entries[3].type) {
                CtscBuffer t3;
                ctsc_buf_init(&t3);
                ctsc_type_to_string(cr->entries[3].type, &t3);
                EXPECT(t3.len == 6 && memcmp(t3.data, "number", 6) == 0);
                ctsc_buf_free(&t3);
            }
        }
        ctsc_arena_free(&a);
    }

    /* Generic class + constructor: types oracle omits MethodDeclaration for
     * `constructor` but still lists parameters (fixtures/checker/generics/06_generic_class.ts;
     * CRLF matches on-disk 101-byte fixture). */
    {
        const char* src = "// @checker: types\r\nclass Box<T> {\r\n  value: T;\r\n  constructor(v: T) {\r\n    this.value = v;\r\n  }\r\n}\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len >= 2) {
            EXPECT(cr->entries[0].decl_kind_name
                   && strcmp(cr->entries[0].decl_kind_name, "PropertyDeclaration") == 0);
            EXPECT(cr->entries[0].pos == 34 && cr->entries[0].end == 43);
            EXPECT(cr->entries[1].decl_kind_name && strcmp(cr->entries[1].decl_kind_name, "Parameter") == 0);
            EXPECT(cr->entries[1].pos == 63 && cr->entries[1].end == 64);
            if (cr->entries[0].type && cr->entries[1].type) {
                CtscBuffer t0, t1;
                ctsc_buf_init(&t0);
                ctsc_buf_init(&t1);
                ctsc_type_to_string(cr->entries[0].type, &t0);
                ctsc_type_to_string(cr->entries[1].type, &t1);
                EXPECT(t0.len == 1 && memcmp(t0.data, "T", 1) == 0);
                EXPECT(t1.len == 1 && memcmp(t1.data, "T", 1) == 0);
                ctsc_buf_free(&t0);
                ctsc_buf_free(&t1);
            }
        }
        ctsc_arena_free(&a);
    }

    /* Instantiated generic class: new Box<number>(1) → Box<number>; b.value → number
     * (fixtures/checker/generics/07_generic_class_usage.ts; checker.ts resolveNewExpression ~37131). */
    {
        const char* src =
            "// @checker: types\n"
            "class Box<T> {\n"
            "  value: T;\n"
            "  constructor(v: T) {\n"
            "    this.value = v;\n"
            "  }\n"
            "}\n"
            "const b = new Box<number>(1);\n"
            "const n = b.value;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 32768);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 4);
        if (cr->entries_len >= 4 && cr->entries[2].type && cr->entries[3].type) {
            CtscBuffer tb, tn;
            ctsc_buf_init(&tb);
            ctsc_buf_init(&tn);
            ctsc_type_to_string(cr->entries[2].type, &tb);
            ctsc_type_to_string(cr->entries[3].type, &tn);
            EXPECT(tb.len == 11 && memcmp(tb.data, "Box<number>", 11) == 0);
            EXPECT(tn.len == 6 && memcmp(tn.data, "number", 6) == 0);
            ctsc_buf_free(&tb);
            ctsc_buf_free(&tn);
        }
        ctsc_arena_free(&a);
    }

    /* Inferred type arguments: new Box(1) → Box<number>; b.value → number
     * (fixtures/checker/generics/08_generic_class_inferred.ts; inferTypeArguments ~35827). */
    {
        const char* src =
            "// @checker: types\n"
            "class Box<T> {\n"
            "  value: T;\n"
            "  constructor(v: T) {\n"
            "    this.value = v;\n"
            "  }\n"
            "}\n"
            "const b = new Box(1);\n"
            "const n = b.value;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 32768);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 4);
        if (cr->entries_len >= 4 && cr->entries[2].type && cr->entries[3].type) {
            CtscBuffer tb, tn;
            ctsc_buf_init(&tb);
            ctsc_buf_init(&tn);
            ctsc_type_to_string(cr->entries[2].type, &tb);
            ctsc_type_to_string(cr->entries[3].type, &tn);
            EXPECT(tb.len == 11 && memcmp(tb.data, "Box<number>", 11) == 0);
            EXPECT(tn.len == 6 && memcmp(tn.data, "number", 6) == 0);
            ctsc_buf_free(&tb);
            ctsc_buf_free(&tn);
        }
        ctsc_arena_free(&a);
    }

    /*
     * Array destructuring: tuple inference from array literal so references get
     * element types (fixtures/checker/destructuring/02_array_destructure.ts;
     * checker.ts getTypeFromBindingPattern ~12433).
     */
    {
        const char* src = "// @checker: types\nconst [a, b] = [1, 2];\nconst c = a;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len >= 1 && cr->entries[0].type) {
            EXPECT(cr->entries[0].type->kind == CTSC_TYPE_NUMBER);
        }
        ctsc_arena_free(&a);
    }

    /*
     * Object destructuring: inferred type of a binding flows to later references
     * (fixtures/checker/destructuring/01_object_destructure.ts; checker.ts
     * checkBindingElement / getTypeFromBindingPattern ~41400+).
     */
    {
        const char* src = "// @checker: types\nconst { x } = { x: 1 };\nconst y = x;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len >= 1 && cr->entries[0].type) {
            EXPECT(cr->entries[0].type->kind == CTSC_TYPE_NUMBER);
        }
        ctsc_arena_free(&a);
    }

    /* Numeric enum member literal type (fixtures/checker/enums/01_numeric_enum.ts;
     * checker.ts getTypeOfPropertyOfType / EnumLiteralType ~35200+). */
    {
        const char* src = "// @checker: types\nenum Color {\n  Red,\n  Green,\n  Blue,\n}\nconst c = Color.Red;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len >= 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 9 && memcmp(ts.data, "Color.Red", 9) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /* String enum member literal (fixtures/checker/enums/02_string_enum.ts). */
    {
        const char* src =
            "// @checker: types\nenum Name {\n  A = \"a\",\n  B = \"b\",\n}\nconst n = Name.A;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 1);
        if (cr->entries_len >= 1 && cr->entries[0].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[0].type, &ts);
            EXPECT(ts.len == 6 && memcmp(ts.data, "Name.A", 6) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    /*
     * `declare namespace N { const x: number; } const y = N.x;`
     * (fixtures/checker/declare/11_declare_namespace_const.ts). The oracle
     * walks the module body so both `x` (inside N) and `y` (top level) must
     * appear in entries, each with type `number`. Mirrors
     * upstream/TypeScript/src/compiler/checker.ts
     * getTypeOfVariableOrParameterOrPropertyWorker (~12537) for the
     * annotated `const x: number` and resolveEntityName (~34822) for `N.x`.
     */
    {
        const char* src =
            "// @checker: types\ndeclare namespace N {\n  const x: number;\n}\nconst y = N.x;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 0);
        EXPECT(cr->entries_len == 2);
        if (cr->entries_len >= 2) {
            EXPECT(cr->entries[0].name_len == 1 && cr->entries[0].name[0] == (uint16_t)'x');
            EXPECT(cr->entries[0].type && cr->entries[0].type->kind == CTSC_TYPE_NUMBER);
            EXPECT(cr->entries[1].name_len == 1 && cr->entries[1].name[0] == (uint16_t)'y');
            EXPECT(cr->entries[1].type && cr->entries[1].type->kind == CTSC_TYPE_NUMBER);
        }
        ctsc_arena_free(&a);
    }

    /*
     * TS2488 on a `for..of` iterable when the iterated expression's type has
     * no `[Symbol.iterator]` (checker.ts checkRightHandSideOfForOf ~45675 →
     * reportTypeNotIterableError ~46169-46185). Under the harness's noLib
     * setup a rest parameter `...nums: number[]` collapses to `{}` because
     * `Array<T>` is unresolved, so `for (const n of nums)` fires TS2488.
     * Mirrors fixtures/checker/rest_params/04_rest_type_mismatch.ts; the
     * same call `sum(1, "two", 3)` must not emit TS2345 because every
     * argument is assignable to `{}` (checker.ts isSimpleTypeRelatedTo's
     * `{}`-target rule, ~22170-22220).
     */
    {
        const char* src =
            "// @checker: diag\r\n"
            "function sum(...nums: number[]): number {\r\n"
            "  let s = 0;\r\n"
            "  for (const n of nums) s += n;\r\n"
            "  return s;\r\n"
            "}\r\n"
            "const r = sum(1, \"two\", 3);\r\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->diagnostics_len == 1);
        if (cr->diagnostics_len >= 1) {
            EXPECT(cr->diagnostics[0].code == 2488);
            /* `nums` identifier in `for (const n of nums)`, start=94, length=4
             * (UTF-16 code units), matching fixtures/checker/rest_params/
             * 04_rest_type_mismatch.ts (CRLF). */
            EXPECT(cr->diagnostics[0].start == 94);
            EXPECT(cr->diagnostics[0].length == 4);
            EXPECT(strcmp(cr->diagnostics[0].message,
                          "Type '{}' must have a '[Symbol.iterator]()' method that returns an iterator.") == 0);
        }
        ctsc_arena_free(&a);
    }

    /*
     * `keyof T` type operator preserved through a type alias (fixtures/
     * checker/keyof/01_keyof_interface.ts). Mirrors upstream/TypeScript/
     * src/compiler/checker.ts getTypeFromTypeOperatorNode (~16940+)
     * KeyOfKeyword → getIndexType; typeToString emits `keyof <operand>`
     * for a non-reduced IndexType (checker.ts typeToTypeNodeHelper
     * IndexType branch). The alias `type K = keyof Point` forwards the
     * IndexType to `declare const k: K` without tagging `K` as the alias
     * name on the result (checker.ts getDeclaredTypeOfTypeAlias does not
     * attach aliasSymbol on non-structural types), so the entry reads
     * `keyof Point` instead of `K`.
     */
    {
        const char* src = "// @checker: types\n"
                          "interface Point {\n"
                          "  x: number;\n"
                          "  y: number;\n"
                          "}\n"
                          "type K = keyof Point;\n"
                          "declare const k: K;\n"
                          "const a = k;\n";
        size_t len = strlen(src);
        CtscArena a;
        ctsc_arena_init(&a, 16384);
        CtscParseResult pr = ctsc_parse(src, len, &a);
        CtscBindResult* br = ctsc_bind(pr.sourceFile, &a);
        CtscCheckResult* cr = ctsc_check(pr.sourceFile, br, &a);
        EXPECT(cr->entries_len == 4);
        /* Entries: Point.x, Point.y, k, a. */
        if (cr->entries_len >= 3 && cr->entries[2].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[2].type, &ts);
            const char* want = "keyof Point";
            EXPECT(ts.len == strlen(want));
            EXPECT(memcmp(ts.data, want, strlen(want)) == 0);
            ctsc_buf_free(&ts);
        }
        if (cr->entries_len >= 4 && cr->entries[3].type) {
            CtscBuffer ts;
            ctsc_buf_init(&ts);
            ctsc_type_to_string(cr->entries[3].type, &ts);
            const char* want = "keyof Point";
            EXPECT(ts.len == strlen(want));
            EXPECT(memcmp(ts.data, want, strlen(want)) == 0);
            ctsc_buf_free(&ts);
        }
        ctsc_arena_free(&a);
    }

    return failed;
}
