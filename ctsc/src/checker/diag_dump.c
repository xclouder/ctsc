#include "ctsc/checker.h"
#include "ctsc/json_writer.h"
#include "ctsc/buffer.h"

#include <stdlib.h>
#include <string.h>

/*
 * JSON emitters for the M4.0 checker. Output shape must match:
 *
 *   oracle-checker-diag.ts  → { "diagnostics": [...], "tsVersion": "5.9.2" }
 *   oracle-checker-types.ts → { "entries":     [...], "tsVersion": "5.9.2" }
 *
 * Agreement with the oracle on `tsVersion` is critical because both oracles
 * attach ts.version and the differ compares strings. The harness caches
 * oracle outputs keyed on ts.version, so when the repo's TypeScript version
 * bumps we want the compare layer (oracle-checker-*.ts) and the ctsc output
 * to both pick up the new version — for ctsc we pass it through at build
 * time (see CMakeLists → CTSC_TS_VERSION define).
 */

#ifndef CTSC_TS_VERSION
/* Fallback: the harness passes the real version via the C preprocessor at
 * build time. If absent we emit an empty string so the differ fails fast
 * with a clear mismatch message rather than silently masking a bug. */
#define CTSC_TS_VERSION ""
#endif

/* ----- diagnostics channel ----- */

static int cmp_diag(const void* a, const void* b) {
    const CtscCheckDiagnostic* x = (const CtscCheckDiagnostic*)a;
    const CtscCheckDiagnostic* y = (const CtscCheckDiagnostic*)b;
    if (x->start != y->start) return x->start - y->start;
    return x->code - y->code;
}

void ctsc_check_dump_diag_json(CtscCheckResult* r, CtscBuffer* out, bool pretty) {
    /*
     * The oracle sorts by (start, code) before emitting. We sort a shallow
     * copy so the in-memory order (insertion) is preserved for callers that
     * need it. Count-wise M4.0 fixtures have <10 diagnostics, so malloc/qsort
     * is plenty.
     */
    CtscCheckDiagnostic* sorted = NULL;
    if (r->diagnostics_len > 0) {
        sorted = (CtscCheckDiagnostic*)malloc(r->diagnostics_len * sizeof(*sorted));
        memcpy(sorted, r->diagnostics, r->diagnostics_len * sizeof(*sorted));
        qsort(sorted, r->diagnostics_len, sizeof(*sorted), cmp_diag);
    }

    CtscJson j; ctsc_json_init(&j, out, pretty);
    ctsc_json_begin_obj(&j);
    ctsc_json_key(&j, "diagnostics");
    ctsc_json_begin_arr(&j);
    for (size_t i = 0; i < r->diagnostics_len; ++i) {
        const CtscCheckDiagnostic* d = &sorted[i];
        ctsc_json_begin_obj(&j);
        ctsc_json_key(&j, "code");        ctsc_json_int(&j, d->code);
        ctsc_json_key(&j, "category");    ctsc_json_cstr(&j, d->category ? d->category : "Error");
        ctsc_json_key(&j, "start");       ctsc_json_int(&j, d->start);
        ctsc_json_key(&j, "length");      ctsc_json_int(&j, d->length);
        ctsc_json_key(&j, "messageText"); ctsc_json_cstr(&j, d->message ? d->message : "");
        ctsc_json_end_obj(&j);
    }
    ctsc_json_end_arr(&j);
    ctsc_json_key(&j, "tsVersion"); ctsc_json_cstr(&j, CTSC_TS_VERSION);
    ctsc_json_end_obj(&j);

    free(sorted);
}

/* ----- types channel ----- */

static int cmp_entry(const void* a, const void* b) {
    const CtscCheckTypeEntry* x = (const CtscCheckTypeEntry*)a;
    const CtscCheckTypeEntry* y = (const CtscCheckTypeEntry*)b;
    if (x->pos != y->pos) return x->pos - y->pos;
    if (x->end != y->end) return x->end - y->end;
    int c = strcmp(x->decl_kind_name ? x->decl_kind_name : "",
                   y->decl_kind_name ? y->decl_kind_name : "");
    if (c != 0) return c;
    /* fall back to name cmp (UTF-16 code-point) */
    size_t nmin = x->name_len < y->name_len ? x->name_len : y->name_len;
    for (size_t i = 0; i < nmin; ++i) {
        if (x->name[i] != y->name[i]) return (int)x->name[i] - (int)y->name[i];
    }
    return (int)x->name_len - (int)y->name_len;
}

void ctsc_check_dump_types_json(CtscCheckResult* r, CtscBuffer* out, bool pretty) {
    CtscCheckTypeEntry* sorted = NULL;
    if (r->entries_len > 0) {
        sorted = (CtscCheckTypeEntry*)malloc(r->entries_len * sizeof(*sorted));
        memcpy(sorted, r->entries, r->entries_len * sizeof(*sorted));
        qsort(sorted, r->entries_len, sizeof(*sorted), cmp_entry);
    }

    CtscJson j; ctsc_json_init(&j, out, pretty);
    ctsc_json_begin_obj(&j);
    ctsc_json_key(&j, "entries");
    ctsc_json_begin_arr(&j);
    for (size_t i = 0; i < r->entries_len; ++i) {
        const CtscCheckTypeEntry* e = &sorted[i];
        ctsc_json_begin_obj(&j);
        ctsc_json_key(&j, "name"); ctsc_json_str_utf16(&j, e->name, e->name_len);
        ctsc_json_key(&j, "kind"); ctsc_json_cstr(&j, e->decl_kind_name ? e->decl_kind_name : "Unknown");
        ctsc_json_key(&j, "pos");  ctsc_json_int(&j, e->pos);
        ctsc_json_key(&j, "end");  ctsc_json_int(&j, e->end);
        /* Format type string. */
        CtscBuffer ts; ctsc_buf_init(&ts);
        if (e->type_string && e->type_string_len > 0) {
            ctsc_buf_append(&ts, e->type_string, e->type_string_len);
        } else {
            ctsc_type_to_string(e->type, &ts);
        }
        ctsc_json_key(&j, "type");
        /* Null-terminated copy onto the stack isn't safe for long types; use
         * ctsc_json_str which accepts (ptr, len) UTF-8 directly. */
        ctsc_json_str(&j, ts.data, ts.len);
        ctsc_buf_free(&ts);
        ctsc_json_end_obj(&j);
    }
    ctsc_json_end_arr(&j);
    ctsc_json_key(&j, "tsVersion"); ctsc_json_cstr(&j, CTSC_TS_VERSION);
    ctsc_json_end_obj(&j);

    free(sorted);
}
