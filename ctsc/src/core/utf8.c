#include "ctsc/utf8.h"

#include <stdlib.h>
#include <string.h>

int32_t ctsc_utf8_decode(const char* p, const char* end, int* out_len) {
    if (p >= end) { if (out_len) { *out_len = 0; } return -1; }
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) { if (out_len) { *out_len = 1; } return c; }
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0)      { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else { if (out_len) { *out_len = 1; } return -1; }
    if (p + n > end) { if (out_len) { *out_len = 1; } return -1; }
    for (int i = 1; i < n; ++i) {
        unsigned char cc = (unsigned char)p[i];
        if ((cc & 0xC0) != 0x80) { if (out_len) { *out_len = 1; } return -1; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (out_len) { *out_len = n; }
    return (int32_t)cp;
}

void ctsc_utf16_init(CtscUtf16Buf* b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void ctsc_utf16_free(CtscUtf16Buf* b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void ctsc_utf16_push(CtscUtf16Buf* b, uint16_t unit) {
    if (b->len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        uint16_t* nd = (uint16_t*)realloc(b->data, cap * sizeof(uint16_t));
        if (!nd) { CTSC_PANIC("utf16: oom"); }
        b->data = nd;
        b->cap = cap;
    }
    b->data[b->len++] = unit;
}

void ctsc_utf16_from_utf8(CtscUtf16Buf* out, const char* src, size_t len) {
    const char* p = src;
    const char* end = src + len;
    while (p < end) {
        int adv = 0;
        int32_t cp = ctsc_utf8_decode(p, end, &adv);
        if (cp < 0) { ctsc_utf16_push(out, 0xFFFD); p += adv ? adv : 1; continue; }
        if (cp <= 0xFFFF) {
            ctsc_utf16_push(out, (uint16_t)cp);
        } else {
            uint32_t v = (uint32_t)cp - 0x10000;
            ctsc_utf16_push(out, (uint16_t)(0xD800 | (v >> 10)));
            ctsc_utf16_push(out, (uint16_t)(0xDC00 | (v & 0x3FF)));
        }
        p += adv;
    }
}

void ctsc_utf16_to_utf8(const uint16_t* src, size_t len, void (*emit)(void*, const char*, size_t), void* ctx) {
    for (size_t i = 0; i < len; ) {
        uint32_t cp = src[i++];
        if (cp >= 0xD800 && cp <= 0xDBFF && i < len) {
            uint16_t lo = src[i];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                i++;
            }
        }
        char buf[4];
        int n;
        if (cp < 0x80)        { buf[0] = (char)cp; n = 1; }
        else if (cp < 0x800)  { buf[0] = (char)(0xC0 | (cp >> 6));
                                buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
        else if (cp < 0x10000){ buf[0] = (char)(0xE0 | (cp >> 12));
                                buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
        else                  { buf[0] = (char)(0xF0 | (cp >> 18));
                                buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                                buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
        emit(ctx, buf, (size_t)n);
    }
}
