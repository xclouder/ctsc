#include "ctsc/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Parser state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* src;
    size_t      pos;
    size_t      len;
    int         line;   /* 1-based */
    int         col;    /* 1-based */
    bool        failed;
    char*       err;
    size_t      err_cap;
} Parser;

static void set_err(Parser* p, const char* msg) {
    if (p->failed) return;
    p->failed = true;
    if (p->err && p->err_cap > 0) {
        snprintf(p->err, p->err_cap, "json: %s at line %d col %d", msg, p->line, p->col);
    }
}

static char peek(Parser* p) {
    return p->pos < p->len ? p->src[p->pos] : '\0';
}

static char advance(Parser* p) {
    if (p->pos >= p->len) return '\0';
    char c = p->src[p->pos++];
    if (c == '\n') { p->line++; p->col = 1; } else { p->col++; }
    return c;
}

static void skip_ws_and_comments(Parser* p) {
    for (;;) {
        char c = peek(p);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(p);
            continue;
        }
        if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            while (p->pos < p->len && peek(p) != '\n') advance(p);
            continue;
        }
        if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '*') {
            advance(p); advance(p);
            while (p->pos < p->len) {
                if (peek(p) == '*' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
                    advance(p); advance(p);
                    break;
                }
                advance(p);
            }
            continue;
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/*  Forward decls                                                     */
/* ------------------------------------------------------------------ */

static bool parse_value(Parser* p, CtscJsonValue* out);

/* ------------------------------------------------------------------ */
/*  String parsing                                                    */
/* ------------------------------------------------------------------ */

static void buf_push(char** buf, size_t* len, size_t* cap, char c) {
    if (*len + 1 >= *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *buf = (char*)realloc(*buf, *cap);
    }
    (*buf)[(*len)++] = c;
}

/* Encode Unicode code point as UTF-8 into buf. */
static void append_codepoint(char** buf, size_t* len, size_t* cap, unsigned cp) {
    if (cp < 0x80) {
        buf_push(buf, len, cap, (char)cp);
    } else if (cp < 0x800) {
        buf_push(buf, len, cap, (char)(0xC0 | (cp >> 6)));
        buf_push(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_push(buf, len, cap, (char)(0xE0 | (cp >> 12)));
        buf_push(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_push(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_push(buf, len, cap, (char)(0xF0 | (cp >> 18)));
        buf_push(buf, len, cap, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_push(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_push(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool parse_string(Parser* p, CtscJsonValue* out) {
    if (advance(p) != '"') { set_err(p, "expected '\"'"); return false; }
    char*  buf = NULL;
    size_t blen = 0, bcap = 0;
    for (;;) {
        if (p->pos >= p->len) { set_err(p, "unterminated string"); free(buf); return false; }
        char c = advance(p);
        if (c == '"') break;
        if (c == '\\') {
            char e = advance(p);
            switch (e) {
                case '"':  buf_push(&buf, &blen, &bcap, '"'); break;
                case '\\': buf_push(&buf, &blen, &bcap, '\\'); break;
                case '/':  buf_push(&buf, &blen, &bcap, '/'); break;
                case 'b':  buf_push(&buf, &blen, &bcap, '\b'); break;
                case 'f':  buf_push(&buf, &blen, &bcap, '\f'); break;
                case 'n':  buf_push(&buf, &blen, &bcap, '\n'); break;
                case 'r':  buf_push(&buf, &blen, &bcap, '\r'); break;
                case 't':  buf_push(&buf, &blen, &bcap, '\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        int h = hex_val(advance(p));
                        if (h < 0) { set_err(p, "bad unicode escape"); free(buf); return false; }
                        cp = (cp << 4) | (unsigned)h;
                    }
                    append_codepoint(&buf, &blen, &bcap, cp);
                    break;
                }
                default:
                    set_err(p, "bad escape");
                    free(buf); return false;
            }
        } else {
            buf_push(&buf, &blen, &bcap, c);
        }
    }
    buf_push(&buf, &blen, &bcap, '\0');
    out->kind = CTSC_JSON_STRING;
    out->u.s.data = buf;
    out->u.s.len  = blen - 1;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Number / keyword                                                  */
/* ------------------------------------------------------------------ */

static bool parse_number(Parser* p, CtscJsonValue* out) {
    size_t start = p->pos;
    if (peek(p) == '-') advance(p);
    while (p->pos < p->len) {
        char c = peek(p);
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            advance(p);
        } else break;
    }
    size_t n = p->pos - start;
    if (n == 0) { set_err(p, "expected number"); return false; }
    char tmp[64];
    if (n >= sizeof(tmp)) { set_err(p, "number too long"); return false; }
    memcpy(tmp, p->src + start, n);
    tmp[n] = '\0';
    char* endp = NULL;
    double v = strtod(tmp, &endp);
    if (endp == tmp) { set_err(p, "bad number"); return false; }
    out->kind = CTSC_JSON_NUMBER;
    out->u.n  = v;
    return true;
}

static bool eat_keyword(Parser* p, const char* kw) {
    size_t n = strlen(kw);
    if (p->pos + n > p->len) return false;
    if (memcmp(p->src + p->pos, kw, n) != 0) return false;
    for (size_t i = 0; i < n; ++i) advance(p);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Array / Object                                                    */
/* ------------------------------------------------------------------ */

static bool parse_array(Parser* p, CtscJsonValue* out) {
    if (advance(p) != '[') { set_err(p, "expected '['"); return false; }
    CtscJsonValue* items = NULL;
    size_t len = 0, cap = 0;
    skip_ws_and_comments(p);
    if (peek(p) == ']') { advance(p); goto done; }
    for (;;) {
        skip_ws_and_comments(p);
        if (peek(p) == ']') { advance(p); break; }  /* trailing comma */
        CtscJsonValue v = {0};
        if (!parse_value(p, &v)) { free(items); return false; }
        if (len >= cap) { cap = cap ? cap * 2 : 4; items = (CtscJsonValue*)realloc(items, cap * sizeof(*items)); }
        items[len++] = v;
        skip_ws_and_comments(p);
        if (peek(p) == ',') { advance(p); continue; }
        if (peek(p) == ']') { advance(p); break; }
        set_err(p, "expected ',' or ']' in array");
        free(items); return false;
    }
done:
    out->kind = CTSC_JSON_ARRAY;
    out->u.arr.items = items;
    out->u.arr.len   = len;
    return true;
}

static bool parse_object(Parser* p, CtscJsonValue* out) {
    if (advance(p) != '{') { set_err(p, "expected '{'"); return false; }
    CtscJsonMember* members = NULL;
    size_t len = 0, cap = 0;
    skip_ws_and_comments(p);
    if (peek(p) == '}') { advance(p); goto done; }
    for (;;) {
        skip_ws_and_comments(p);
        if (peek(p) == '}') { advance(p); break; }
        if (peek(p) != '"') { set_err(p, "object key must be string"); goto fail; }
        CtscJsonValue keyVal = {0};
        if (!parse_string(p, &keyVal)) goto fail;
        skip_ws_and_comments(p);
        if (advance(p) != ':') { set_err(p, "expected ':' after key"); free(keyVal.u.s.data); goto fail; }
        skip_ws_and_comments(p);
        CtscJsonValue val = {0};
        if (!parse_value(p, &val)) { free(keyVal.u.s.data); goto fail; }
        if (len >= cap) { cap = cap ? cap * 2 : 4; members = (CtscJsonMember*)realloc(members, cap * sizeof(*members)); }
        members[len].key   = keyVal.u.s.data;   /* take ownership */
        members[len].value = val;
        len++;
        skip_ws_and_comments(p);
        if (peek(p) == ',') { advance(p); continue; }
        if (peek(p) == '}') { advance(p); break; }
        set_err(p, "expected ',' or '}' in object");
        goto fail;
    }
done:
    out->kind = CTSC_JSON_OBJECT;
    out->u.obj.members = members;
    out->u.obj.len     = len;
    return true;
fail:
    if (members) {
        for (size_t i = 0; i < len; ++i) {
            free(members[i].key);
            ctsc_json_free(&members[i].value);
        }
        free(members);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Dispatcher                                                        */
/* ------------------------------------------------------------------ */

static bool parse_value(Parser* p, CtscJsonValue* out) {
    skip_ws_and_comments(p);
    char c = peek(p);
    if (c == '"') return parse_string(p, out);
    if (c == '{') return parse_object(p, out);
    if (c == '[') return parse_array(p, out);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p, out);
    if (c == 't') {
        if (!eat_keyword(p, "true")) { set_err(p, "bad literal"); return false; }
        out->kind = CTSC_JSON_BOOL; out->u.b = true; return true;
    }
    if (c == 'f') {
        if (!eat_keyword(p, "false")) { set_err(p, "bad literal"); return false; }
        out->kind = CTSC_JSON_BOOL; out->u.b = false; return true;
    }
    if (c == 'n') {
        if (!eat_keyword(p, "null")) { set_err(p, "bad literal"); return false; }
        out->kind = CTSC_JSON_NULL; return true;
    }
    set_err(p, "unexpected character");
    return false;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void ctsc_json_free(CtscJsonValue* v) {
    if (!v) return;
    switch (v->kind) {
        case CTSC_JSON_STRING:
            free(v->u.s.data);
            break;
        case CTSC_JSON_ARRAY:
            for (size_t i = 0; i < v->u.arr.len; ++i) ctsc_json_free(&v->u.arr.items[i]);
            free(v->u.arr.items);
            break;
        case CTSC_JSON_OBJECT:
            for (size_t i = 0; i < v->u.obj.len; ++i) {
                free(v->u.obj.members[i].key);
                ctsc_json_free(&v->u.obj.members[i].value);
            }
            free(v->u.obj.members);
            break;
        default:
            break;
    }
    /* Caller owns the top-level container itself. */
    v->kind = CTSC_JSON_NULL;
}

CtscJsonValue* ctsc_json_parse(const char* src, size_t len, char* err_out, size_t err_cap) {
    if (err_out && err_cap) err_out[0] = '\0';
    Parser p = { src, 0, len, 1, 1, false, err_out, err_cap };
    CtscJsonValue* root = (CtscJsonValue*)calloc(1, sizeof(*root));
    if (!parse_value(&p, root)) {
        ctsc_json_free(root);
        free(root);
        return NULL;
    }
    skip_ws_and_comments(&p);
    if (p.pos < p.len) {
        set_err(&p, "trailing data after root value");
        ctsc_json_free(root);
        free(root);
        return NULL;
    }
    return root;
}

const CtscJsonValue* ctsc_json_obj_get(const CtscJsonValue* obj, const char* key) {
    if (!obj || obj->kind != CTSC_JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->u.obj.len; ++i) {
        if (strcmp(obj->u.obj.members[i].key, key) == 0) return &obj->u.obj.members[i].value;
    }
    return NULL;
}

const char* ctsc_json_as_cstr(const CtscJsonValue* v) {
    if (!v || v->kind != CTSC_JSON_STRING) return NULL;
    return v->u.s.data;
}

bool ctsc_json_as_bool(const CtscJsonValue* v, bool dflt) {
    if (!v || v->kind != CTSC_JSON_BOOL) return dflt;
    return v->u.b;
}
