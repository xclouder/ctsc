#ifndef CTSC_STRING_REF_H
#define CTSC_STRING_REF_H

#include "common.h"

typedef struct {
    const char* data;
    size_t      len;
} CtscStringRef;

#define CTSC_STR_LIT(s) ((CtscStringRef){ (s), sizeof(s) - 1 })

static inline CtscStringRef ctsc_str(const char* data, size_t len) {
    CtscStringRef r;
    r.data = data;
    r.len = len;
    return r;
}

static inline CtscStringRef ctsc_str_z(const char* s) {
    CtscStringRef r;
    r.data = s;
    size_t n = 0;
    if (s) { while (s[n]) { n++; } }
    r.len = n;
    return r;
}

bool   ctsc_str_eq(CtscStringRef a, CtscStringRef b);
bool   ctsc_str_eq_z(CtscStringRef a, const char* z);
size_t ctsc_str_hash(CtscStringRef s);

#endif
