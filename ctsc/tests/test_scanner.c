#include "ctsc/scanner.h"
#include "ctsc/arena.h"
#include <stdio.h>
#include <string.h>

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)

/*
 * For a NumericLiteral scanned from `src`, return the effective tokenValue
 * (scanner.current.value when the scanner decided to normalise, else the raw
 * text). Compare against `expected` as ASCII.
 */
static int expect_numeric_value(const char* src, const char* expected) {
    int failed = 0;
    CtscArena arena; ctsc_arena_init(&arena, 4096);
    CtscDiagnosticList diags; ctsc_diag_init(&diags);
    CtscScanner s; ctsc_scanner_init(&s, src, strlen(src), &arena, &diags);
    CtscSyntaxKind k = ctsc_scanner_next(&s);
    if (k != CTSC_SK_NumericLiteral) {
        fprintf(stderr, "FAIL numeric(%s): expected NumericLiteral got %s\n", src, ctsc_syntax_kind_name(k));
        failed++;
    } else {
        const uint16_t* got = s.current.value ? s.current.value : s.current.text;
        size_t got_len    = s.current.value ? s.current.value_len : s.current.text_len;
        size_t exp_len = strlen(expected);
        int match = (got_len == exp_len);
        for (size_t i = 0; match && i < exp_len; i++) {
            if (got[i] != (uint16_t)(uint8_t)expected[i]) { match = 0; }
        }
        if (!match) {
            fprintf(stderr, "FAIL numeric(%s): expected \"%s\" got \"", src, expected);
            for (size_t i = 0; i < got_len; i++) { fputc((char)got[i], stderr); }
            fprintf(stderr, "\"\n");
            failed++;
        }
    }
    ctsc_scanner_free(&s);
    ctsc_diag_free(&diags);
    ctsc_arena_free(&arena);
    return failed;
}

static int expect_kinds(const char* src, const CtscSyntaxKind* expected, size_t n) {
    int failed = 0;
    CtscArena arena; ctsc_arena_init(&arena, 4096);
    CtscDiagnosticList diags; ctsc_diag_init(&diags);
    CtscScanner s; ctsc_scanner_init(&s, src, strlen(src), &arena, &diags);
    for (size_t i = 0; i < n; ++i) {
        CtscSyntaxKind k = ctsc_scanner_next(&s);
        if (k != expected[i]) {
            fprintf(stderr, "FAIL token %zu: expected %s got %s (src=%s)\n", i,
                ctsc_syntax_kind_name(expected[i]), ctsc_syntax_kind_name(k), src);
            failed++;
        }
    }
    ctsc_scanner_free(&s);
    ctsc_diag_free(&diags);
    ctsc_arena_free(&arena);
    return failed;
}

int test_scanner(void) {
    int failed = 0;

    CtscSyntaxKind k1[] = {
        CTSC_SK_ConstKeyword, CTSC_SK_Identifier, CTSC_SK_EqualsToken,
        CTSC_SK_NumericLiteral, CTSC_SK_SemicolonToken, CTSC_SK_EndOfFileToken
    };
    failed += expect_kinds("const x = 42;", k1, sizeof(k1)/sizeof(k1[0]));

    CtscSyntaxKind k2[] = {
        CTSC_SK_LetKeyword, CTSC_SK_Identifier, CTSC_SK_EqualsToken,
        CTSC_SK_StringLiteral, CTSC_SK_SemicolonToken, CTSC_SK_EndOfFileToken
    };
    failed += expect_kinds("let s = \"hi\";", k2, sizeof(k2)/sizeof(k2[0]));

    CtscSyntaxKind k3[] = {
        CTSC_SK_FunctionKeyword, CTSC_SK_Identifier,
        CTSC_SK_OpenParenToken, CTSC_SK_CloseParenToken,
        CTSC_SK_OpenBraceToken,
        CTSC_SK_ReturnKeyword, CTSC_SK_TrueKeyword, CTSC_SK_SemicolonToken,
        CTSC_SK_CloseBraceToken, CTSC_SK_EndOfFileToken
    };
    failed += expect_kinds("function f() { return true; }", k3, sizeof(k3)/sizeof(k3[0]));

    CtscSyntaxKind k4[] = {
        CTSC_SK_Identifier, CTSC_SK_EqualsEqualsEqualsToken, CTSC_SK_Identifier,
        CTSC_SK_EndOfFileToken
    };
    failed += expect_kinds("a === b", k4, sizeof(k4)/sizeof(k4[0]));

    /* Legacy octal integer stops before '.'; fraction is a separate numeric literal (scanner.ts scanNumber). */
    CtscSyntaxKind k5[] = {
        CTSC_SK_NumericLiteral, CTSC_SK_NumericLiteral, CTSC_SK_EndOfFileToken
    };
    failed += expect_kinds("01.0", k5, sizeof(k5)/sizeof(k5[0]));

    /* Empty template literal (upstream scanner.ts scanTemplateAndSetTokenValue). */
    CtscSyntaxKind k6[] = {
        CTSC_SK_NoSubstitutionTemplateLiteral, CTSC_SK_EndOfFileToken
    };
    failed += expect_kinds("``", k6, sizeof(k6)/sizeof(k6[0]));

    /* Numeric tokenValue coercion (upstream scanner.ts scanNumber ~1250/1308/1315
     * `tokenValue = "" + +result`). */
    failed += expect_numeric_value("0", "0");
    failed += expect_numeric_value("1", "1");
    failed += expect_numeric_value("42", "42");
    failed += expect_numeric_value(".0", "0");
    failed += expect_numeric_value(".5", "0.5");
    failed += expect_numeric_value("1.0", "1");
    failed += expect_numeric_value("1.5", "1.5");
    failed += expect_numeric_value("1e0", "1");
    failed += expect_numeric_value("1e+0", "1");
    failed += expect_numeric_value("1e", "1");
    failed += expect_numeric_value("1e+", "1");
    failed += expect_numeric_value("01", "1");         /* LegacyOctal */
    failed += expect_numeric_value("09", "9");         /* NonOctalDecimalIntegerLiteral */
    failed += expect_numeric_value("098.5", "98.5");   /* ContainsLeadingZero + decimal */

    /* Identifier with `\uXXXX` escape in the middle: the raw `text` span
     * covers the escape, while the decoded `value` holds the resolved name.
     * Mirrors upstream scanner.ts scanIdentifier (~2425) +
     * scanIdentifierParts (~1781); see fixtures/scanner/from-upstream/
     * 107_parserClassDeclaration23.ts. */
    {
        CtscSyntaxKind ident_escape[] = {
            CTSC_SK_ClassKeyword, CTSC_SK_Identifier, CTSC_SK_OpenBraceToken,
            CTSC_SK_CloseBraceToken, CTSC_SK_EndOfFileToken
        };
        failed += expect_kinds("class C\\u0032 {}", ident_escape, sizeof(ident_escape)/sizeof(ident_escape[0]));
    }

    /* Identifier that begins with a `\uXXXX` escape (main scan switch
     * backslash path, mirrors scanner.ts case backslash ~2306). \u0043 -> 'C'. */
    {
        CtscArena arena; ctsc_arena_init(&arena, 4096);
        CtscDiagnosticList diags; ctsc_diag_init(&diags);
        const char* src = "\\u0043";
        CtscScanner s; ctsc_scanner_init(&s, src, strlen(src), &arena, &diags);
        CtscSyntaxKind k = ctsc_scanner_next(&s);
        EXPECT(k == CTSC_SK_Identifier);
        /* Raw text span is the 6-char escape. */
        EXPECT(s.current.text_len == 6);
        /* Decoded value is 'C'. */
        EXPECT(s.current.value != NULL && s.current.value_len == 1 && s.current.value[0] == 'C');
        ctsc_scanner_free(&s);
        ctsc_diag_free(&diags);
        ctsc_arena_free(&arena);
    }

    /* A `\uXXXX` escape whose decoded value spells a keyword: the scanner
     * must still classify it as the keyword (upstream getIdentifierToken
     * ~1815 keys off tokenValue). "\u0069f" == "if". */
    {
        CtscArena arena; ctsc_arena_init(&arena, 4096);
        CtscDiagnosticList diags; ctsc_diag_init(&diags);
        const char* src = "\\u0069f";
        CtscScanner s; ctsc_scanner_init(&s, src, strlen(src), &arena, &diags);
        CtscSyntaxKind k = ctsc_scanner_next(&s);
        EXPECT(k == CTSC_SK_IfKeyword);
        ctsc_scanner_free(&s);
        ctsc_diag_free(&diags);
        ctsc_arena_free(&arena);
    }

    return failed;
}
