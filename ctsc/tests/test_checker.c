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

    return failed;
}
