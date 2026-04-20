#ifndef CTSC_JSON_H
#define CTSC_JSON_H

/*
 * Minimal JSON parser for reading tsconfig.json in ctsc --project.
 *
 * Scope: just enough to parse the TypeScript compiler's tsconfig files.
 *   - Supports: null, bool, number (as double), string, array, object.
 *   - Accepts // and /+...+/ comments (tsconfig is documented as JSONC).
 *   - Accepts trailing commas in arrays/objects (also JSONC).
 *   - Does NOT support Unicode escapes beyond \uXXXX BMP, unicode NaN/Inf,
 *     or non-string keys (tsconfig never needs these).
 *
 * Allocation strategy: malloc-based. Suitable for small, short-lived
 * documents. Callers must ctsc_json_free() the root value.
 */

#include "common.h"

typedef enum {
    CTSC_JSON_NULL = 0,
    CTSC_JSON_BOOL,
    CTSC_JSON_NUMBER,
    CTSC_JSON_STRING,
    CTSC_JSON_ARRAY,
    CTSC_JSON_OBJECT
} CtscJsonKind;

typedef struct CtscJsonValue  CtscJsonValue;
typedef struct CtscJsonMember CtscJsonMember;

struct CtscJsonValue {
    CtscJsonKind kind;
    union {
        bool   b;
        double n;
        struct { char*  data;    size_t len; } s;   /* string bytes, NUL-terminated */
        struct { CtscJsonValue*  items;   size_t len; } arr;
        struct { CtscJsonMember* members; size_t len; } obj;
    } u;
};

struct CtscJsonMember {
    char*         key;   /* NUL-terminated */
    CtscJsonValue value;
};

/*
 * Parse `src` (utf8, `len` bytes) as JSONC. Returns a heap-allocated root
 * CtscJsonValue on success (free via ctsc_json_free) or NULL on failure;
 * on failure, `err_out` (size `err_cap`) receives a human-readable message
 * when non-NULL.
 */
CtscJsonValue* ctsc_json_parse(const char* src, size_t len, char* err_out, size_t err_cap);

/* Recursively release v and everything it owns. Safe on NULL. */
void           ctsc_json_free(CtscJsonValue* v);

/*
 * Object lookup: returns the first member whose key equals `key`, or NULL
 * if not found or `obj` is not an object. Returned pointer aliases obj's
 * internal storage; do not free.
 */
const CtscJsonValue* ctsc_json_obj_get(const CtscJsonValue* obj, const char* key);

/* Convenience: returns the string contents of v, or NULL. */
const char* ctsc_json_as_cstr(const CtscJsonValue* v);

/* Convenience: returns v->u.b if v is a bool, else `dflt`. */
bool        ctsc_json_as_bool(const CtscJsonValue* v, bool dflt);

#endif
