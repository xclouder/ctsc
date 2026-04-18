#ifndef CTSC_UTF8_H
#define CTSC_UTF8_H

#include "common.h"

/*
 * TypeScript strings are UTF-16 code unit sequences. The scanner operates on
 * char codes where each index advances by one UTF-16 code unit. To preserve
 * offset compatibility with tsc, callers should internally convert source
 * bytes (UTF-8) into UTF-16 code units.
 *
 * For the Phase 0 bootstrap we provide UTF-8 decoding and UTF-16 conversion
 * helpers; token positions reported by ctsc MUST be in UTF-16 code units so
 * oracle diffs stay byte-exact.
 */

typedef struct {
    const uint16_t* data;
    size_t          len;
} CtscUtf16View;

typedef struct {
    uint16_t* data;
    size_t    len;
    size_t    cap;
} CtscUtf16Buf;

int32_t ctsc_utf8_decode(const char* p, const char* end, int* out_len);

void    ctsc_utf16_init(CtscUtf16Buf* b);
void    ctsc_utf16_free(CtscUtf16Buf* b);
void    ctsc_utf16_push(CtscUtf16Buf* b, uint16_t unit);
void    ctsc_utf16_from_utf8(CtscUtf16Buf* out, const char* src, size_t len);

/* Append a UTF-16 range to a UTF-8 byte buffer (for JSON emission). */
void    ctsc_utf16_to_utf8(const uint16_t* src, size_t len, void (*emit)(void*, const char*, size_t), void* ctx);

#endif
