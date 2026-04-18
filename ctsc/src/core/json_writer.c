#include "ctsc/json_writer.h"
#include "ctsc/utf8.h"

#include <stdio.h>
#include <string.h>

static void emit_raw(CtscJson* j, const char* s, size_t n) {
    ctsc_buf_append(j->out, s, n);
}
static void emit_cstr(CtscJson* j, const char* s) {
    ctsc_buf_append_cstr(j->out, s);
}

static void indent(CtscJson* j) {
    if (!j->pretty) { return; }
    ctsc_buf_append_char(j->out, '\n');
    for (int i = 0; i < j->depth; ++i) { ctsc_buf_append_cstr(j->out, "  "); }
}

static void push_container(CtscJson* j) {
    if (j->depth >= 63) { CTSC_PANIC("json: nested too deep"); }
    j->first_mask |= ((uint64_t)1 << j->depth);
    j->depth++;
}

static void pop_container(CtscJson* j) {
    if (j->depth == 0) { CTSC_PANIC("json: unbalanced"); }
    j->depth--;
    j->first_mask &= ~((uint64_t)1 << j->depth);
}

static bool is_first(CtscJson* j) {
    if (j->depth == 0) { return true; }
    return (j->first_mask & ((uint64_t)1 << (j->depth - 1))) != 0;
}

static void clear_first(CtscJson* j) {
    if (j->depth > 0) {
        j->first_mask &= ~((uint64_t)1 << (j->depth - 1));
    }
}

static void before_value(CtscJson* j) {
    if (j->depth == 0) { return; }
    if (is_first(j)) { clear_first(j); indent(j); return; }
    ctsc_buf_append_char(j->out, ',');
    indent(j);
}

static void write_escaped_codeunit(CtscJson* j, uint16_t u);

static void emit_str_body(CtscJson* j, const char* s, size_t n) {
    emit_raw(j, "\"", 1);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        write_escaped_codeunit(j, (uint16_t)c);
    }
    emit_raw(j, "\"", 1);
}

static void emit_str_body_u16(CtscJson* j, const uint16_t* data, size_t len) {
    emit_raw(j, "\"", 1);
    for (size_t i = 0; i < len; ++i) {
        write_escaped_codeunit(j, data[i]);
    }
    emit_raw(j, "\"", 1);
}

void ctsc_json_init(CtscJson* j, CtscBuffer* out, bool pretty) {
    j->out = out;
    j->indent = 0;
    j->first_mask = 0;
    j->depth = 0;
    j->pretty = pretty;
}

void ctsc_json_begin_obj(CtscJson* j) { before_value(j); emit_cstr(j, "{"); push_container(j); }
void ctsc_json_end_obj(CtscJson* j)   { bool first = is_first(j); pop_container(j); if (!first) { indent(j); } emit_cstr(j, "}"); }
void ctsc_json_begin_arr(CtscJson* j) { before_value(j); emit_cstr(j, "["); push_container(j); }
void ctsc_json_end_arr(CtscJson* j)   { bool first = is_first(j); pop_container(j); if (!first) { indent(j); } emit_cstr(j, "]"); }

void ctsc_json_key(CtscJson* j, const char* key) {
    before_value(j);
    emit_str_body(j, key, strlen(key));
    emit_cstr(j, j->pretty ? ": " : ":");
    /* Mark next value as first-in-pair (no comma / indent). */
    if (j->depth > 0) { j->first_mask |= ((uint64_t)1 << (j->depth - 1)); }
}

static void write_escaped_codeunit(CtscJson* j, uint16_t u) {
    char buf[8];
    if (u == '"')         { emit_raw(j, "\\\"", 2); return; }
    if (u == '\\')        { emit_raw(j, "\\\\", 2); return; }
    if (u == '\b')        { emit_raw(j, "\\b", 2); return; }
    if (u == '\f')        { emit_raw(j, "\\f", 2); return; }
    if (u == '\n')        { emit_raw(j, "\\n", 2); return; }
    if (u == '\r')        { emit_raw(j, "\\r", 2); return; }
    if (u == '\t')        { emit_raw(j, "\\t", 2); return; }
    if (u < 0x20 || (u >= 0xD800 && u <= 0xDFFF)) {
        snprintf(buf, sizeof(buf), "\\u%04x", u);
        emit_raw(j, buf, strlen(buf));
        return;
    }
    if (u < 0x80) {
        char c = (char)u;
        emit_raw(j, &c, 1);
        return;
    }
    if (u < 0x800) {
        buf[0] = (char)(0xC0 | (u >> 6));
        buf[1] = (char)(0x80 | (u & 0x3F));
        emit_raw(j, buf, 2);
        return;
    }
    buf[0] = (char)(0xE0 | (u >> 12));
    buf[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (u & 0x3F));
    emit_raw(j, buf, 3);
}

void ctsc_json_str(CtscJson* j, const char* s, size_t n) {
    before_value(j);
    emit_str_body(j, s, n);
}

void ctsc_json_cstr(CtscJson* j, const char* s) {
    ctsc_json_str(j, s, strlen(s));
}

void ctsc_json_str_utf16(CtscJson* j, const uint16_t* data, size_t len) {
    before_value(j);
    emit_str_body_u16(j, data, len);
}

void ctsc_json_int(CtscJson* j, long long v) {
    before_value(j);
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", v);
    emit_raw(j, buf, (size_t)n);
}

void ctsc_json_bool(CtscJson* j, bool v) {
    before_value(j);
    emit_cstr(j, v ? "true" : "false");
}

void ctsc_json_null(CtscJson* j) {
    before_value(j);
    emit_cstr(j, "null");
}
