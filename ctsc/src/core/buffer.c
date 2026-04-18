#include "ctsc/buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ctsc_buf_init(CtscBuffer* b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void ctsc_buf_free(CtscBuffer* b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void ctsc_buf_reserve(CtscBuffer* b, size_t min_cap) {
    if (b->cap >= min_cap) { return; }
    size_t cap = b->cap ? b->cap : 64;
    while (cap < min_cap) { cap *= 2; }
    char* nd = (char*)realloc(b->data, cap);
    if (!nd) { CTSC_PANIC("buffer: oom %zu", cap); }
    b->data = nd;
    b->cap = cap;
}

void ctsc_buf_append(CtscBuffer* b, const char* s, size_t n) {
    ctsc_buf_reserve(b, b->len + n + 1);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void ctsc_buf_append_cstr(CtscBuffer* b, const char* s) {
    ctsc_buf_append(b, s, strlen(s));
}

void ctsc_buf_append_char(CtscBuffer* b, char c) {
    ctsc_buf_reserve(b, b->len + 2);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void ctsc_buf_printf(CtscBuffer* b, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); CTSC_PANIC("buf_printf: encoding error"); }
    ctsc_buf_reserve(b, b->len + (size_t)n + 1);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

char* ctsc_read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) { *out_len = got; }
    return buf;
}
