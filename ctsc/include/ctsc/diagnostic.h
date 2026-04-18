#ifndef CTSC_DIAGNOSTIC_H
#define CTSC_DIAGNOSTIC_H

#include "common.h"

typedef enum {
    CTSC_DIAG_ERROR = 1,
    CTSC_DIAG_WARNING = 0,
    CTSC_DIAG_MESSAGE = 2,
    CTSC_DIAG_SUGGESTION = 3
} CtscDiagCategory;

typedef struct CtscDiagnostic {
    struct CtscDiagnostic* next;
    CtscDiagCategory       category;
    int                    code;
    int                    start; /* UTF-16 code-unit offset */
    int                    length;
    char*                  message; /* heap-owned */
} CtscDiagnostic;

typedef struct {
    CtscDiagnostic* head;
    CtscDiagnostic* tail;
    size_t          count;
} CtscDiagnosticList;

void ctsc_diag_init(CtscDiagnosticList* l);
void ctsc_diag_free(CtscDiagnosticList* l);
void ctsc_diag_push(CtscDiagnosticList* l, CtscDiagCategory cat, int code, int start, int length, const char* fmt, ...);
/*
 * Drop all diagnostics after the first `keep`. Used to roll back diagnostics
 * emitted during a speculative parse (mirrors upstream parser.ts tryParse /
 * speculationHelper).
 */
void ctsc_diag_truncate(CtscDiagnosticList* l, size_t keep);

#endif
