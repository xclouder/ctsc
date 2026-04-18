#include "ctsc/binder.h"
#include "ctsc/parser.h"
#include "ctsc/arena.h"
#include "ctsc/buffer.h"

#include <stdio.h>
#include <string.h>

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)

static int str_contains(const CtscBuffer* b, const char* needle) {
    size_t n = strlen(needle);
    if (b->len < n) return 0;
    for (size_t i = 0; i + n <= b->len; ++i) {
        if (memcmp(b->data + i, needle, n) == 0) return 1;
    }
    return 0;
}

int test_binder(void) {
    int failed = 0;

    /* Empty source file produces a single SourceFile scope with empty
     * symbols. Mirrors harness/fixtures/binder/basic/01_empty.ts and the
     * expected output from harness/src/oracle-binder.ts. */
    {
        CtscArena a; ctsc_arena_init(&a, 4096);
        CtscParseResult r = ctsc_parse("", 0, &a);
        CtscBuffer out; ctsc_buf_init(&out);
        ctsc_bindings_dump_json(r.sourceFile, &out, /*pretty*/ false);
        const char* expected = "{\"scopes\":[{\"kind\":\"SourceFile\",\"pos\":0,\"end\":0,\"symbols\":[]}],\"diagnostics\":[]}";
        if (out.len != strlen(expected) || memcmp(out.data, expected, out.len) != 0) {
            fprintf(stderr, "FAIL %s:%d: empty-file bindings JSON mismatch\n", __FILE__, __LINE__);
            fprintf(stderr, "  expected: %s\n", expected);
            fprintf(stderr, "  actual:   %.*s\n", (int)out.len, out.data);
            failed++;
        }
        ctsc_buf_free(&out);
        ctsc_arena_free(&a);
    }

    /* Sanity: a trailing-semicolon-only program still produces exactly one
     * scope and no symbols — ensures we do not accidentally recurse into
     * EmptyStatement as a container. */
    {
        CtscArena a; ctsc_arena_init(&a, 4096);
        CtscParseResult r = ctsc_parse(";", 1, &a);
        CtscBuffer out; ctsc_buf_init(&out);
        ctsc_bindings_dump_json(r.sourceFile, &out, /*pretty*/ false);
        EXPECT(str_contains(&out, "\"kind\":\"SourceFile\""));
        EXPECT(str_contains(&out, "\"symbols\":[]"));
        /* Exactly one scope object. */
        size_t scope_count = 0;
        for (size_t i = 0; i + 7 <= out.len; ++i) {
            if (memcmp(out.data + i, "\"kind\":", 7) == 0) scope_count++;
        }
        EXPECT(scope_count == 1);
        ctsc_buf_free(&out);
        ctsc_arena_free(&a);
    }

    /* FunctionDeclaration + parameters. Mirrors
     * fixtures/binder/nested/01_function_params.ts: the SourceFile scope
     * holds an entry for `f` (SymbolFlags.Function), and a second scope for
     * the FunctionDeclaration itself holds `a` / `b`
     * (SymbolFlags.FunctionScopedVariable). Sort-by-name means `a` precedes
     * `b` in the emitted JSON. */
    {
        const char* src = "function f(a, b) {\n  return a;\n}\n";
        size_t len = strlen(src);
        CtscArena a; ctsc_arena_init(&a, 8192);
        CtscParseResult r = ctsc_parse(src, len, &a);
        CtscBuffer out; ctsc_buf_init(&out);
        ctsc_bindings_dump_json(r.sourceFile, &out, /*pretty*/ false);
        const char* expected =
            "{\"scopes\":["
              "{\"kind\":\"SourceFile\",\"pos\":0,\"end\":33,\"symbols\":["
                "{\"name\":\"f\",\"flags\":[\"Function\"],\"decls\":["
                  "{\"kind\":\"FunctionDeclaration\",\"pos\":0,\"end\":32}]}]},"
              "{\"kind\":\"FunctionDeclaration\",\"pos\":0,\"end\":32,\"symbols\":["
                "{\"name\":\"a\",\"flags\":[\"FunctionScopedVariable\"],\"decls\":["
                  "{\"kind\":\"Parameter\",\"pos\":11,\"end\":12}]},"
                "{\"name\":\"b\",\"flags\":[\"FunctionScopedVariable\"],\"decls\":["
                  "{\"kind\":\"Parameter\",\"pos\":13,\"end\":15}]}]}"
            "],\"diagnostics\":[]}";
        if (out.len != strlen(expected) || memcmp(out.data, expected, out.len) != 0) {
            fprintf(stderr, "FAIL %s:%d: function+params bindings JSON mismatch\n", __FILE__, __LINE__);
            fprintf(stderr, "  expected: %s\n", expected);
            fprintf(stderr, "  actual:   %.*s\n", (int)out.len, out.data);
            failed++;
        }
        ctsc_buf_free(&out);
        ctsc_arena_free(&a);
    }

    /* Top-level `let` binding. Mirrors
     * fixtures/binder/top-level/01_let.ts: the SourceFile scope holds `a`
     * tagged SymbolFlags.BlockScopedVariable (since SourceFile is both the
     * container and the block scope for top-level code), with a single
     * VariableDeclaration decl spanning `a = 1`. See
     * upstream/TypeScript/src/compiler/binder.ts bindVariableDeclarationOrBindingElement (~3648). */
    {
        const char* src = "let a = 1;\n";
        size_t len = strlen(src);
        CtscArena a; ctsc_arena_init(&a, 4096);
        CtscParseResult r = ctsc_parse(src, len, &a);
        CtscBuffer out; ctsc_buf_init(&out);
        ctsc_bindings_dump_json(r.sourceFile, &out, /*pretty*/ false);
        const char* expected =
            "{\"scopes\":[{\"kind\":\"SourceFile\",\"pos\":0,\"end\":11,\"symbols\":["
              "{\"name\":\"a\",\"flags\":[\"BlockScopedVariable\"],\"decls\":["
                "{\"kind\":\"VariableDeclaration\",\"pos\":3,\"end\":9}]}]}"
            "],\"diagnostics\":[]}";
        if (out.len != strlen(expected) || memcmp(out.data, expected, out.len) != 0) {
            fprintf(stderr, "FAIL %s:%d: top-level let bindings JSON mismatch\n", __FILE__, __LINE__);
            fprintf(stderr, "  expected: %s\n", expected);
            fprintf(stderr, "  actual:   %.*s\n", (int)out.len, out.data);
            failed++;
        }
        ctsc_buf_free(&out);
        ctsc_arena_free(&a);
    }

    /* Top-level `var` binding. `var` is function-scoped; at top level the
     * SourceFile is the enclosing function/container, so `b` lands there
     * tagged SymbolFlags.FunctionScopedVariable (see binder.ts ~3679). */
    {
        const char* src = "var b = 2;\n";
        size_t len = strlen(src);
        CtscArena a; ctsc_arena_init(&a, 4096);
        CtscParseResult r = ctsc_parse(src, len, &a);
        CtscBuffer out; ctsc_buf_init(&out);
        ctsc_bindings_dump_json(r.sourceFile, &out, /*pretty*/ false);
        const char* expected =
            "{\"scopes\":[{\"kind\":\"SourceFile\",\"pos\":0,\"end\":11,\"symbols\":["
              "{\"name\":\"b\",\"flags\":[\"FunctionScopedVariable\"],\"decls\":["
                "{\"kind\":\"VariableDeclaration\",\"pos\":3,\"end\":9}]}]}"
            "],\"diagnostics\":[]}";
        if (out.len != strlen(expected) || memcmp(out.data, expected, out.len) != 0) {
            fprintf(stderr, "FAIL %s:%d: top-level var bindings JSON mismatch\n", __FILE__, __LINE__);
            fprintf(stderr, "  expected: %s\n", expected);
            fprintf(stderr, "  actual:   %.*s\n", (int)out.len, out.data);
            failed++;
        }
        ctsc_buf_free(&out);
        ctsc_arena_free(&a);
    }

    return failed;
}
