#ifndef CTSC_JSON_WRITER_H
#define CTSC_JSON_WRITER_H

#include "buffer.h"

typedef struct {
    CtscBuffer* out;
    int         indent;
    /* Stack of "first element?" flags, up to 64 nesting levels. */
    uint64_t    first_mask;
    int         depth;
    bool        pretty;
} CtscJson;

void ctsc_json_init(CtscJson* j, CtscBuffer* out, bool pretty);

void ctsc_json_begin_obj(CtscJson* j);
void ctsc_json_end_obj(CtscJson* j);
void ctsc_json_begin_arr(CtscJson* j);
void ctsc_json_end_arr(CtscJson* j);

void ctsc_json_key(CtscJson* j, const char* key);

void ctsc_json_str(CtscJson* j, const char* s, size_t n);
void ctsc_json_cstr(CtscJson* j, const char* s);
void ctsc_json_int(CtscJson* j, long long v);
void ctsc_json_bool(CtscJson* j, bool v);
void ctsc_json_null(CtscJson* j);

/* Emit a string from UTF-16 code units (what the scanner produces). */
void ctsc_json_str_utf16(CtscJson* j, const uint16_t* data, size_t len);

#endif
