#include "ctsc/scanner.h"
#include "ctsc/parser.h"
#include "ctsc/binder.h"
#include "ctsc/checker.h"
#include "ctsc/emitter.h"
#include "ctsc/arena.h"
#include "ctsc/buffer.h"
#include "ctsc/utf8.h"
#include "ctsc/project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void print_usage(FILE* f) {
    fprintf(f,
        "ctsc - C port of tsc (harness-driven)\n"
        "\n"
        "Usage: ctsc <command> [options] <file>\n"
        "\n"
        "Commands:\n"
        "  --dump-tokens     Dump scanner token stream as JSON\n"
        "  --dump-ast        Dump parser AST as JSON\n"
        "  --dump-bindings   Dump binder output as JSON\n"
        "  --dump-types      Dump checker type entries as JSON\n"
        "  --check           Dump checker semantic diagnostics as JSON\n"
        "  --emit            Emit JavaScript for a single source file (stdout)\n"
        "  --project <path>  Compile a tsconfig-driven project (file or dir)\n"
        "\n"
        "Options:\n"
        "  --pretty          Pretty-print JSON\n"
        "  -o <path>         Write output to file instead of stdout\n"
        "  --no-package-json Skip writing dist/package.json in --project mode\n"
        "  --verbose         Print progress to stderr\n"
        "  -h, --help        Show this help\n"
    );
}

typedef enum {
    CMD_NONE,
    CMD_DUMP_TOKENS,
    CMD_DUMP_AST,
    CMD_DUMP_BINDINGS,
    CMD_DUMP_TYPES,
    CMD_CHECK,
    CMD_EMIT,
    CMD_PROJECT
} Cmd;

int main(int argc, char** argv) {
#ifdef _WIN32
    /* Ensure binary mode: tsc oracle outputs LF; we must not let CRT translate
     * our LFs to CRLFs when writing to stdout. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    Cmd cmd = CMD_NONE;
    const char* input = NULL;
    const char* output = NULL;
    bool pretty = false;
    bool write_package_json = true;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { print_usage(stdout); return 0; }
        if (strcmp(a, "--dump-tokens") == 0)   { cmd = CMD_DUMP_TOKENS;   continue; }
        if (strcmp(a, "--dump-ast") == 0)      { cmd = CMD_DUMP_AST;      continue; }
        if (strcmp(a, "--dump-bindings") == 0) { cmd = CMD_DUMP_BINDINGS; continue; }
        if (strcmp(a, "--dump-types") == 0)    { cmd = CMD_DUMP_TYPES;    continue; }
        if (strcmp(a, "--check") == 0)         { cmd = CMD_CHECK;         continue; }
        if (strcmp(a, "--emit") == 0)          { cmd = CMD_EMIT;          continue; }
        if (strcmp(a, "--project") == 0) {
            cmd = CMD_PROJECT;
            if (i + 1 >= argc) { fprintf(stderr, "error: --project requires a path\n"); return 2; }
            input = argv[++i];
            continue;
        }
        if (strcmp(a, "--pretty") == 0)      { pretty = true;         continue; }
        if (strcmp(a, "--no-package-json") == 0) { write_package_json = false; continue; }
        if (strcmp(a, "--verbose") == 0)     { verbose = true;        continue; }
        if (strcmp(a, "-o") == 0)            { if (i + 1 >= argc) { fprintf(stderr, "error: -o requires a path\n"); return 2; } output = argv[++i]; continue; }
        if (a[0] == '-') { fprintf(stderr, "error: unknown option '%s'\n", a); return 2; }
        if (input) { fprintf(stderr, "error: multiple input files\n"); return 2; }
        input = a;
    }

    if (cmd == CMD_NONE) { print_usage(stderr); return 2; }
    if (!input)          { fprintf(stderr, "error: missing input file\n"); return 2; }

    if (cmd == CMD_PROJECT) {
        CtscProjectOptions popts = { input, write_package_json, verbose };
        return ctsc_run_project(&popts);
    }

    size_t src_len = 0;
    char*  src = ctsc_read_file(input, &src_len);
    if (!src) { fprintf(stderr, "error: cannot read '%s'\n", input); return 1; }

    CtscBuffer out; ctsc_buf_init(&out);
    int rc = 0;
    switch (cmd) {
        case CMD_DUMP_TOKENS:
            ctsc_scanner_dump_tokens_json(src, src_len, &out, pretty);
            break;
        case CMD_DUMP_AST: {
            CtscArena a; ctsc_arena_init(&a, 64 * 1024);
            CtscParseResult r = ctsc_parse(src, src_len, &a);
            ctsc_ast_dump_json(r.sourceFile, &out, pretty);
            ctsc_arena_free(&a);
            break;
        }
        case CMD_DUMP_BINDINGS: {
            CtscArena a; ctsc_arena_init(&a, 64 * 1024);
            CtscParseResult r = ctsc_parse(src, src_len, &a);
            ctsc_bindings_dump_json(r.sourceFile, &out, pretty);
            ctsc_arena_free(&a);
            break;
        }
        case CMD_DUMP_TYPES: {
            CtscArena a; ctsc_arena_init(&a, 64 * 1024);
            CtscParseResult r = ctsc_parse(src, src_len, &a);
            CtscBindResult* b = ctsc_bind(r.sourceFile, &a);
            CtscCheckResult* c = ctsc_check(r.sourceFile, b, &a);
            ctsc_check_dump_types_json(c, &out, pretty);
            ctsc_arena_free(&a);
            break;
        }
        case CMD_CHECK: {
            CtscArena a; ctsc_arena_init(&a, 64 * 1024);
            CtscParseResult r = ctsc_parse(src, src_len, &a);
            CtscBindResult* b = ctsc_bind(r.sourceFile, &a);
            CtscCheckResult* c = ctsc_check(r.sourceFile, b, &a);
            ctsc_check_dump_diag_json(c, &out, pretty);
            ctsc_arena_free(&a);
            break;
        }
        case CMD_EMIT: {
            CtscArena a; ctsc_arena_init(&a, 64 * 1024);
            CtscParseResult r = ctsc_parse(src, src_len, &a);
            /* The emitter needs UTF-16 source to replay leading comment trivia
             * for files whose statements list is empty (see emitter.c). */
            CtscUtf16Buf u16; ctsc_utf16_init(&u16);
            ctsc_utf16_from_utf8(&u16, src, src_len);
            ctsc_emit_js(r.sourceFile, &u16, &out);
            ctsc_utf16_free(&u16);
            ctsc_arena_free(&a);
            break;
        }
        default: break;
    }

    if (rc == 0) {
        if (output) {
            FILE* of = fopen(output, "wb");
            if (!of) { fprintf(stderr, "error: cannot write '%s'\n", output); rc = 1; }
            else     { fwrite(out.data, 1, out.len, of); fclose(of); }
        } else {
            fwrite(out.data, 1, out.len, stdout);
            if (pretty && out.len && out.data[out.len - 1] != '\n') { fputc('\n', stdout); }
        }
    }

    ctsc_buf_free(&out);
    free(src);
    return rc;
}
