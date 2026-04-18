#include "ctsc/diagnostic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ctsc_diag_init(CtscDiagnosticList* l) {
    l->head = NULL;
    l->tail = NULL;
    l->count = 0;
}

void ctsc_diag_free(CtscDiagnosticList* l) {
    CtscDiagnostic* d = l->head;
    while (d) {
        CtscDiagnostic* n = d->next;
        free(d->message);
        free(d);
        d = n;
    }
    l->head = NULL;
    l->tail = NULL;
    l->count = 0;
}

/*
 * Truncate the list so that only the first `keep` diagnostics remain. Used by
 * speculative parse helpers (mirrors upstream parser.ts tryParse/speculationHelper
 * which rolls back diagnostics emitted during the trial when the trial fails).
 */
void ctsc_diag_truncate(CtscDiagnosticList* l, size_t keep) {
    if (keep >= l->count) return;
    CtscDiagnostic* d = l->head;
    size_t i = 0;
    CtscDiagnostic* new_tail = NULL;
    while (d && i < keep) {
        new_tail = d;
        d = d->next;
        i++;
    }
    /* d is the first node to drop. */
    while (d) {
        CtscDiagnostic* n = d->next;
        free(d->message);
        free(d);
        d = n;
    }
    if (new_tail) {
        new_tail->next = NULL;
        l->tail = new_tail;
    } else {
        l->head = NULL;
        l->tail = NULL;
    }
    l->count = keep;
}

void ctsc_diag_push(CtscDiagnosticList* l, CtscDiagCategory cat, int code, int start, int length, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    char* msg = (char*)malloc((size_t)n + 1);
    if (!msg) { va_end(ap2); CTSC_PANIC("diag: oom"); }
    vsnprintf(msg, (size_t)n + 1, fmt, ap2);
    va_end(ap2);

    CtscDiagnostic* d = (CtscDiagnostic*)calloc(1, sizeof(CtscDiagnostic));
    if (!d) { free(msg); CTSC_PANIC("diag: oom"); }
    d->category = cat;
    d->code = code;
    d->start = start;
    d->length = length;
    d->message = msg;

    if (l->tail) { l->tail->next = d; } else { l->head = d; }
    l->tail = d;
    l->count++;
}
