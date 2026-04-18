#ifndef CTSC_BUFFER_H
#define CTSC_BUFFER_H

#include "common.h"

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} CtscBuffer;

void ctsc_buf_init(CtscBuffer* b);
void ctsc_buf_free(CtscBuffer* b);
void ctsc_buf_reserve(CtscBuffer* b, size_t min_cap);
void ctsc_buf_append(CtscBuffer* b, const char* s, size_t n);
void ctsc_buf_append_cstr(CtscBuffer* b, const char* s);
void ctsc_buf_append_char(CtscBuffer* b, char c);
void ctsc_buf_printf(CtscBuffer* b, const char* fmt, ...);

char*  ctsc_read_file(const char* path, size_t* out_len);

#endif
