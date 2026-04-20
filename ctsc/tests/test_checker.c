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

    return failed;
}
