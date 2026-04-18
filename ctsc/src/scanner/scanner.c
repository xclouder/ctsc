#include "ctsc/scanner.h"
#include "ctsc/arena.h"
#include "ctsc/json_writer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Minimal Phase-1 scanner. The harness diffs its token stream against tsc's.
 * As the agent loop extends this, keep behaviour in 1:1 with
 * upstream/TypeScript/src/compiler/scanner.ts (scan()).
 */

static uint16_t peek(const CtscScanner* s, size_t off) {
    size_t i = s->pos + off;
    if (i >= s->source.len) { return 0; }
    return s->source.data[i];
}

static bool is_line_break(uint16_t c) {
    return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029;
}

static bool is_whitespace_single_line(uint16_t c) {
    return c == 0x20 || c == 0x09 || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF;
}

static bool is_digit(uint16_t c) { return c >= '0' && c <= '9'; }

static bool is_octal_digit(uint16_t c) { return c >= '0' && c <= '7'; }

static bool is_hex_digit(uint16_t c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_digit_value(uint16_t c) {
    if (is_digit(c)) { return (int)(c - '0'); }
    if (c >= 'a' && c <= 'f') { return (int)(c - 'a' + 10); }
    if (c >= 'A' && c <= 'F') { return (int)(c - 'A' + 10); }
    return -1;
}

/*
 * Returns true iff a decimal point or exponent part was consumed. Needed so
 * scan_number can mirror tsc's `decimalFragment !== undefined || Scientific`
 * branch in scanNumber (upstream scanner.ts ~1312) which coerces tokenValue
 * via `"" + +result`.
 */
static bool scan_number_fraction_and_exponent(CtscScanner* s) {
    bool saw = false;
    if (s->pos < s->source.len && s->source.data[s->pos] == '.') {
        s->pos++;
        saw = true;
        while (s->pos < s->source.len && is_digit(s->source.data[s->pos])) { s->pos++; }
    }
    if (s->pos < s->source.len && (s->source.data[s->pos] == 'e' || s->source.data[s->pos] == 'E')) {
        s->pos++;
        saw = true;
        if (s->pos < s->source.len && (s->source.data[s->pos] == '+' || s->source.data[s->pos] == '-')) { s->pos++; }
        while (s->pos < s->source.len && is_digit(s->source.data[s->pos])) { s->pos++; }
    }
    return saw;
}

/*
 * Format `v` using ECMA-262 6.1.6.1.13 Number::toString (the "shortest
 * round-trip" decimal). We iterate k = 1..17 and take the first significand
 * length that strtod() reparses exactly to `v`. This mirrors the observable
 * behaviour of `"" + (+source)` that tsc invokes in scanNumber (upstream
 * scanner.ts ~1250, ~1308, ~1315).
 *
 * Writes up to `cap - 1` ASCII bytes plus a NUL. Returns number of bytes
 * written (excluding NUL). `v` is assumed non-negative and finite; callers
 * handle NaN / Infinity / sign separately (tsc numeric literals never produce
 * them here).
 */
static size_t ecma_format_nonneg_double(double v, char* out, size_t cap) {
    if (cap == 0) { return 0; }
    if (v == 0.0) {
        if (cap < 2) { out[0] = 0; return 0; }
        out[0] = '0'; out[1] = 0; return 1;
    }

    /* Find shortest significand length k in [1, 17] that round-trips. */
    char sig[18];
    int k = 17;
    int decexp = 0; /* exponent of the leading digit (so s * 10^(decexp-k+1) = v). */
    for (int ki = 1; ki <= 17; ki++) {
        char tmp[48];
        int n = snprintf(tmp, sizeof(tmp), "%.*e", ki - 1, v);
        if (n <= 0 || (size_t)n >= sizeof(tmp)) { continue; }
        double rv = strtod(tmp, NULL);
        if (rv != v) { continue; }

        /* Parse tmp = "d[.ddd]e[+-]NN" */
        int di = 0;
        const char* p = tmp;
        if (*p == '-' || *p == '+') { p++; } /* v >= 0 so no '-' but defensive */
        sig[di++] = *p++;
        if (*p == '.') {
            p++;
            while (*p && *p != 'e' && *p != 'E' && di < 17) {
                sig[di++] = *p++;
            }
            while (*p && *p != 'e' && *p != 'E') { p++; }
        }
        int exp_val = 0, exp_sign = 1;
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '+') { p++; }
            else if (*p == '-') { exp_sign = -1; p++; }
            while (*p >= '0' && *p <= '9') {
                exp_val = exp_val * 10 + (*p - '0');
                p++;
            }
        }
        exp_val *= exp_sign;

        /* Strip trailing zeros from the significand. */
        while (di > 1 && sig[di - 1] == '0') { di--; }
        k = di;
        decexp = exp_val;
        break;
    }

    /* ECMA-262: let n = decexp + 1 so that 10^(n-1) <= s < 10^n and
     * m = s * 10^(n - k). */
    int n_exp = decexp + 1;

    size_t out_len = 0;
    #define PUSH(ch) do { if (out_len + 1 < cap) { out[out_len++] = (char)(ch); } } while (0)

    if (k <= n_exp && n_exp <= 21) {
        for (int i = 0; i < k; i++) { PUSH(sig[i]); }
        for (int i = 0; i < n_exp - k; i++) { PUSH('0'); }
    }
    else if (0 < n_exp && n_exp <= 21) {
        for (int i = 0; i < n_exp; i++) { PUSH(sig[i]); }
        PUSH('.');
        for (int i = n_exp; i < k; i++) { PUSH(sig[i]); }
    }
    else if (-6 < n_exp && n_exp <= 0) {
        PUSH('0');
        PUSH('.');
        for (int i = 0; i < -n_exp; i++) { PUSH('0'); }
        for (int i = 0; i < k; i++) { PUSH(sig[i]); }
    }
    else {
        PUSH(sig[0]);
        if (k > 1) {
            PUSH('.');
            for (int i = 1; i < k; i++) { PUSH(sig[i]); }
        }
        PUSH('e');
        int e = n_exp - 1;
        if (e >= 0) { PUSH('+'); }
        else { PUSH('-'); e = -e; }
        char ebuf[16];
        int el = snprintf(ebuf, sizeof(ebuf), "%d", e);
        for (int i = 0; i < el; i++) { PUSH(ebuf[i]); }
    }

    if (out_len < cap) { out[out_len] = 0; } else { out[cap - 1] = 0; }
    #undef PUSH
    return out_len;
}

/*
 * Compute and store the numeric tokenValue for the UTF-16 source range
 * [start, end). Mirrors tsc's `tokenValue = "" + +result` (upstream
 * scanner.ts ~1308 / ~1315): parse as a JS Number and format via
 * Number::toString. The range is expected to be an ASCII-only numeric
 * lexeme (digits / '.' / 'e' / 'E' / sign); any '_' separators have already
 * been rejected here (ctsc does not yet scan them).
 */
static void set_numeric_coerced_value(CtscScanner* s, size_t start, size_t end) {
    char buf[64];
    size_t n = end - start;
    if (n >= sizeof(buf)) { n = sizeof(buf) - 1; }
    for (size_t i = 0; i < n; i++) {
        uint16_t c = s->source.data[start + i];
        buf[i] = (c < 0x80) ? (char)c : '?';
    }
    buf[n] = 0;

    double v = strtod(buf, NULL);
    if (!isfinite(v)) { v = 0.0; }
    if (v < 0.0) { v = -v; }

    char out[48];
    size_t out_len = ecma_format_nonneg_double(v, out, sizeof(out));

    CtscArena* arena = (CtscArena*)s->arena_ptr;
    uint16_t* vbuf = (uint16_t*)ctsc_arena_alloc_aligned(arena, out_len * sizeof(uint16_t), sizeof(uint16_t));
    for (size_t i = 0; i < out_len; i++) { vbuf[i] = (uint16_t)(uint8_t)out[i]; }
    s->current.value = vbuf;
    s->current.value_len = out_len;
}

/* Decimal literal starting with '.' (see upstream scanner.ts scan() case '.' : isDigit).
 * This path always has a decimal fragment, so tsc's scanNumber (~1312) coerces
 * tokenValue via `"" + +result`. */
static void scan_number_leading_dot(CtscScanner* s) {
    size_t start = s->pos;
    s->pos++;
    while (s->pos < s->source.len && is_digit(s->source.data[s->pos])) { s->pos++; }
    if (s->pos < s->source.len && (s->source.data[s->pos] == 'e' || s->source.data[s->pos] == 'E')) {
        s->pos++;
        if (s->pos < s->source.len && (s->source.data[s->pos] == '+' || s->source.data[s->pos] == '-')) { s->pos++; }
        while (s->pos < s->source.len && is_digit(s->source.data[s->pos])) { s->pos++; }
    }
    s->current.kind = CTSC_SK_NumericLiteral;
    s->current.text = s->source.data + start;
    s->current.text_len = s->pos - start;
    set_numeric_coerced_value(s, start, s->pos);
}

static bool is_identifier_start(uint16_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
}

static bool is_identifier_part(uint16_t c) {
    return is_identifier_start(c) || is_digit(c);
}

void ctsc_scanner_init(CtscScanner* s, const char* src, size_t len, struct CtscArena* arena, CtscDiagnosticList* diags) {
    memset(s, 0, sizeof(*s));
    ctsc_utf16_init(&s->source);
    ctsc_utf16_from_utf8(&s->source, src, len);
    s->pos = 0;
    s->include_trivia = false;
    s->diagnostics = diags;
    s->arena_ptr = arena;
    s->current.kind = CTSC_SK_Unknown;
}

void ctsc_scanner_free(CtscScanner* s) {
    ctsc_utf16_free(&s->source);
}

static void skip_trivia(CtscScanner* s) {
    s->precedingLineBreak = false;
    while (s->pos < s->source.len) {
        uint16_t c = s->source.data[s->pos];
        if (is_line_break(c)) {
            s->precedingLineBreak = true;
            if (c == 0x0D && s->pos + 1 < s->source.len && s->source.data[s->pos + 1] == 0x0A) {
                s->pos += 2;
            } else {
                s->pos += 1;
            }
            continue;
        }
        if (is_whitespace_single_line(c)) { s->pos++; continue; }
        if (c == '/' && s->pos + 1 < s->source.len) {
            uint16_t n = s->source.data[s->pos + 1];
            if (n == '/') {
                s->pos += 2;
                while (s->pos < s->source.len && !is_line_break(s->source.data[s->pos])) { s->pos++; }
                continue;
            }
            if (n == '*') {
                s->pos += 2;
                while (s->pos + 1 < s->source.len && !(s->source.data[s->pos] == '*' && s->source.data[s->pos + 1] == '/')) {
                    if (is_line_break(s->source.data[s->pos])) { s->precedingLineBreak = true; }
                    s->pos++;
                }
                if (s->pos + 1 < s->source.len) {
                    s->pos += 2;
                } else if (s->diagnostics) {
                    ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1010, (int)s->pos, 0, "'*/' expected.");
                }
                continue;
            }
        }
        break;
    }
}

static bool is_white_space_like(uint16_t c) {
    return is_whitespace_single_line(c) || is_line_break(c);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts reScanSlashToken (~2467).
 * Simplified: no scanRegularExpressionWorker / flag validation; positions and
 * unterminated handling follow the first pass and trailing trim.
 */
CtscSyntaxKind ctsc_scanner_re_scan_slash_token(CtscScanner* s) {
    if (s->current.kind != CTSC_SK_SlashToken && s->current.kind != CTSC_SK_SlashEqualsToken) {
        return s->current.kind;
    }
    int token_start = s->current.start;
    size_t start_of_reg_exp_body = (size_t)token_start + 1;
    s->pos = start_of_reg_exp_body;

    bool in_escape = false;
    bool named_capture_groups = false;
    bool in_character_class = false;
    bool unterminated = false;

    for (;;) {
        if (s->pos >= s->source.len) {
            unterminated = true;
            break;
        }
        uint16_t ch = s->source.data[s->pos];
        if (is_line_break(ch)) {
            unterminated = true;
            break;
        }

        if (in_escape) {
            in_escape = false;
        } else if (ch == '/' && !in_character_class) {
            break;
        } else if (ch == '[') {
            in_character_class = true;
        } else if (ch == '\\') {
            in_escape = true;
        } else if (ch == ']') {
            in_character_class = false;
        } else if (!in_character_class && ch == '(' && s->pos + 3 < s->source.len
            && s->source.data[s->pos + 1] == '?'
            && s->source.data[s->pos + 2] == '<'
            && s->source.data[s->pos + 3] != '='
            && s->source.data[s->pos + 3] != '!') {
            named_capture_groups = true;
        }
        (void)named_capture_groups;
        s->pos++;
    }

    size_t end_of_reg_exp_body = s->pos;

    if (unterminated) {
        /* Recovery pass mirrors scanner.ts ~2528 (simplified: bracket walk omitted). */
        s->pos = start_of_reg_exp_body;
        in_escape = false;
        size_t character_class_depth = 0;
        bool in_decimal_quantifier = false;
        size_t group_depth = 0;
        while (s->pos < end_of_reg_exp_body) {
            uint16_t ch = s->source.data[s->pos];
            if (in_escape) {
                in_escape = false;
            } else if (ch == '\\') {
                in_escape = true;
            } else if (ch == '[') {
                character_class_depth++;
            } else if (ch == ']' && character_class_depth) {
                character_class_depth--;
            } else if (!character_class_depth) {
                if (ch == '{') {
                    in_decimal_quantifier = true;
                } else if (ch == '}' && in_decimal_quantifier) {
                    in_decimal_quantifier = false;
                } else if (!in_decimal_quantifier) {
                    if (ch == '(') {
                        group_depth++;
                    } else if (ch == ')' && group_depth) {
                        group_depth--;
                    } else if (ch == ')' || ch == ']' || ch == '}') {
                        break;
                    }
                }
            }
            s->pos++;
        }
        while (s->pos > start_of_reg_exp_body
            && (is_white_space_like(s->source.data[s->pos - 1]) || s->source.data[s->pos - 1] == ';')) {
            s->pos--;
        }
        if (s->diagnostics) {
            ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1161, token_start, (int)(s->pos - (size_t)token_start),
                "Unterminated regular expression literal.");
        }
    } else {
        /* Consume closing '/'. */
        if (s->pos < s->source.len && s->source.data[s->pos] == '/') {
            s->pos++;
        }
        /* Flags */
        while (s->pos < s->source.len) {
            uint16_t ch = s->source.data[s->pos];
            if (!is_identifier_part(ch)) {
                break;
            }
            s->pos++;
        }
    }

    s->current.kind = CTSC_SK_RegularExpressionLiteral;
    s->current.start = token_start;
    s->current.end = (int)s->pos;
    s->current.text = s->source.data + (size_t)token_start;
    s->current.text_len = (size_t)s->pos - (size_t)token_start;
    return s->current.kind;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts reScanGreaterToken (~2438).
 * The scanner always emits a bare '>' so nested generics like `Array<Array<T>>`
 * remain parsable; binary-expression / assignment parsing invokes this helper
 * to coalesce the following '>' / '=' characters into '>>', '>>>', '>=',
 * '>>=' or '>>>=' operators.
 *
 * Note `s->pos` points just past the current '>' token, so we probe the bytes
 * at pos, pos+1, pos+2 (same layout as upstream's pos / pos+1 / pos+2 probes,
 * because upstream's `pos` there is also positioned just past the bare '>').
 */
CtscSyntaxKind ctsc_scanner_re_scan_greater_token(CtscScanner* s) {
    if (s->current.kind != CTSC_SK_GreaterThanToken) {
        return s->current.kind;
    }
    if (s->pos < s->source.len && s->source.data[s->pos] == '>') {
        if (s->pos + 1 < s->source.len && s->source.data[s->pos + 1] == '>') {
            /* '>>>' ... optionally '>>>=' when followed by '=' (upstream
             * scanner.ts ~2442-2445). */
            if (s->pos + 2 < s->source.len && s->source.data[s->pos + 2] == '=') {
                s->pos += 3;
                s->current.kind = CTSC_SK_GreaterThanGreaterThanGreaterThanEqualsToken;
                s->current.end = (int)s->pos;
                return s->current.kind;
            }
            s->pos += 2;
            s->current.kind = CTSC_SK_GreaterThanGreaterThanGreaterThanToken;
            s->current.end = (int)s->pos;
            return s->current.kind;
        }
        if (s->pos + 1 < s->source.len && s->source.data[s->pos + 1] == '=') {
            /* '>>=' (upstream scanner.ts ~2447-2448). */
            s->pos += 2;
            s->current.kind = CTSC_SK_GreaterThanGreaterThanEqualsToken;
            s->current.end = (int)s->pos;
            return s->current.kind;
        }
        /* '>>' (GreaterThanGreaterThanToken). */
        s->pos += 1;
        s->current.kind = CTSC_SK_GreaterThanGreaterThanToken;
        s->current.end = (int)s->pos;
        return s->current.kind;
    }
    if (s->pos < s->source.len && s->source.data[s->pos] == '=') {
        s->pos += 1;
        s->current.kind = CTSC_SK_GreaterThanEqualsToken;
        s->current.end = (int)s->pos;
        return s->current.kind;
    }
    return s->current.kind;
}

/* Forward decls: the value_* helpers live near scan_string/scan_template. */
static void value_ensure_cap(CtscScanner* s, uint16_t** buf, size_t len, size_t* cap, size_t need);
static void value_push_u16(CtscScanner* s, uint16_t** buf, size_t* len, size_t* cap, uint16_t c);
static void value_append_utf16_range(CtscScanner* s, uint16_t** buf, size_t* len, size_t* cap,
    const uint16_t* data, size_t n);

/*
 * Peek a `\uXXXX` Unicode escape at s->pos and return the decoded 16-bit code
 * unit (or -1 if not a valid escape). Mirrors upstream scanner.ts
 * peekUnicodeEscape (~1758). Does not advance s->pos.
 */
static int peek_unicode_escape(CtscScanner* s) {
    if (s->pos + 5 >= s->source.len) { return -1; }
    if (s->source.data[s->pos] != '\\' || s->source.data[s->pos + 1] != 'u') { return -1; }
    if (s->source.data[s->pos + 2] == '{') { return -1; }
    int v = 0;
    for (int k = 0; k < 4; k++) {
        uint16_t c = s->source.data[s->pos + 2 + k];
        if (!is_hex_digit(c)) { return -1; }
        v = (v << 4) | hex_digit_value(c);
    }
    return v;
}

/*
 * Peek a `\u{XXXX}` extended Unicode escape at s->pos and return the decoded
 * code point (0..0x10FFFF), or -1 if invalid. Mirrors upstream scanner.ts
 * peekExtendedUnicodeEscape (~1769). Does not advance s->pos.
 */
static int peek_extended_unicode_escape(CtscScanner* s) {
    if (s->pos + 3 >= s->source.len) { return -1; }
    if (s->source.data[s->pos] != '\\' || s->source.data[s->pos + 1] != 'u'
        || s->source.data[s->pos + 2] != '{') { return -1; }
    size_t p = s->pos + 3;
    if (p >= s->source.len || !is_hex_digit(s->source.data[p])) { return -1; }
    int v = 0;
    while (p < s->source.len && is_hex_digit(s->source.data[p])) {
        v = (v << 4) | hex_digit_value(s->source.data[p]);
        if (v > 0x10FFFF) { return -1; }
        p++;
    }
    return v;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts scanIdentifierParts (~1781):
 * consume identifier parts starting at s->pos, resolving `\uXXXX` and
 * `\u{XXXX}` escapes whose decoded code point is a valid identifier part.
 * Appends decoded UTF-16 code units to *buf. Returns when the next source
 * character is neither an identifier part nor a valid identifier escape.
 */
static void scan_identifier_parts_into(CtscScanner* s, uint16_t** buf, size_t* len, size_t* cap) {
    size_t start = s->pos;
    while (s->pos < s->source.len) {
        uint16_t ch = s->source.data[s->pos];
        if (is_identifier_part(ch)) {
            s->pos++;
            continue;
        }
        if (ch == '\\') {
            int ext = peek_extended_unicode_escape(s);
            if (ext >= 0 && (ext <= 0xFFFF ? is_identifier_part((uint16_t)ext) : false)) {
                /* Flush pending raw range, then consume the escape and append decoded. */
                value_append_utf16_range(s, buf, len, cap, s->source.data + start, s->pos - start);
                /* Advance past the escape: '\' 'u' '{' hex+ '}' */
                s->pos += 3;
                int v = 0;
                while (s->pos < s->source.len && is_hex_digit(s->source.data[s->pos])) {
                    v = (v << 4) | hex_digit_value(s->source.data[s->pos++]);
                }
                if (s->pos < s->source.len && s->source.data[s->pos] == '}') { s->pos++; }
                if (v <= 0xFFFF) {
                    value_push_u16(s, buf, len, cap, (uint16_t)v);
                } else {
                    v -= 0x10000;
                    value_push_u16(s, buf, len, cap, (uint16_t)(0xD800 + (v >> 10)));
                    value_push_u16(s, buf, len, cap, (uint16_t)(0xDC00 + (v & 0x3FF)));
                }
                start = s->pos;
                continue;
            }
            int simple = peek_unicode_escape(s);
            if (simple >= 0 && is_identifier_part((uint16_t)simple)) {
                value_append_utf16_range(s, buf, len, cap, s->source.data + start, s->pos - start);
                s->pos += 6;
                value_push_u16(s, buf, len, cap, (uint16_t)simple);
                start = s->pos;
                continue;
            }
            break;
        }
        break;
    }
    value_append_utf16_range(s, buf, len, cap, s->source.data + start, s->pos - start);
}

static void scan_identifier(CtscScanner* s) {
    size_t start = s->pos;
    s->pos++;
    while (s->pos < s->source.len && is_identifier_part(s->source.data[s->pos])) { s->pos++; }
    s->current.text = s->source.data + start;
    s->current.text_len = s->pos - start;
    /* Fast path: no escape. tokenValue == raw text. */
    if (s->pos >= s->source.len || s->source.data[s->pos] != '\\') {
        s->current.kind = ctsc_identifier_or_keyword(s->current.text, s->current.text_len);
        return;
    }
    /* Slow path: an identifier part `\uXXXX` escape may follow. Mirrors
     * upstream scanner.ts scanIdentifier (~2431) which switches to
     * scanIdentifierParts to decode escapes into tokenValue. */
    CtscArena* arena = (CtscArena*)s->arena_ptr;
    size_t cap = 16;
    uint16_t* vbuf = (uint16_t*)ctsc_arena_alloc_aligned(arena, cap * sizeof(uint16_t), sizeof(uint16_t));
    size_t vlen = 0;
    value_append_utf16_range(s, &vbuf, &vlen, &cap, s->source.data + start, s->pos - start);
    scan_identifier_parts_into(s, &vbuf, &vlen, &cap);
    s->current.text_len = s->pos - start;
    s->current.value = vbuf;
    s->current.value_len = vlen;
    s->current.kind = ctsc_identifier_or_keyword(vbuf, vlen);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts scan() case '\\' (~2306):
 * an identifier may begin with a `\uXXXX` or `\u{XXXX}` escape whose decoded
 * code point is a valid IdentifierStart. If so, emit Identifier (or keyword)
 * with text = raw source slice and value = decoded name. Otherwise, report
 * `Invalid character` and produce Unknown (caller advances one unit).
 */
static bool try_scan_identifier_from_backslash(CtscScanner* s) {
    size_t start = s->pos;
    int ext = peek_extended_unicode_escape(s);
    int simple = -1;
    bool take_ext = false;
    if (ext >= 0 && ext <= 0xFFFF && is_identifier_start((uint16_t)ext)) {
        take_ext = true;
    } else {
        simple = peek_unicode_escape(s);
        if (!(simple >= 0 && is_identifier_start((uint16_t)simple))) {
            return false;
        }
    }
    CtscArena* arena = (CtscArena*)s->arena_ptr;
    size_t cap = 16;
    uint16_t* vbuf = (uint16_t*)ctsc_arena_alloc_aligned(arena, cap * sizeof(uint16_t), sizeof(uint16_t));
    size_t vlen = 0;
    if (take_ext) {
        s->pos += 3;
        int v = 0;
        while (s->pos < s->source.len && is_hex_digit(s->source.data[s->pos])) {
            v = (v << 4) | hex_digit_value(s->source.data[s->pos++]);
        }
        if (s->pos < s->source.len && s->source.data[s->pos] == '}') { s->pos++; }
        if (v <= 0xFFFF) {
            value_push_u16(s, &vbuf, &vlen, &cap, (uint16_t)v);
        } else {
            v -= 0x10000;
            value_push_u16(s, &vbuf, &vlen, &cap, (uint16_t)(0xD800 + (v >> 10)));
            value_push_u16(s, &vbuf, &vlen, &cap, (uint16_t)(0xDC00 + (v & 0x3FF)));
        }
    } else {
        s->pos += 6;
        value_push_u16(s, &vbuf, &vlen, &cap, (uint16_t)simple);
    }
    scan_identifier_parts_into(s, &vbuf, &vlen, &cap);
    s->current.text = s->source.data + start;
    s->current.text_len = s->pos - start;
    s->current.value = vbuf;
    s->current.value_len = vlen;
    s->current.kind = ctsc_identifier_or_keyword(vbuf, vlen);
    return true;
}

/*
 * Write a u64 as a decimal ASCII string into a UTF-16 arena buffer. Returns
 * the allocated buffer; *out_len receives the number of UTF-16 code units.
 * Used to mirror tsc's `tokenValue = "" + parseInt(octalDigits, 8)` for
 * LegacyOctalIntegerLiteral (upstream scanner.ts ~1258).
 */
static uint16_t* u64_to_utf16_decimal(CtscArena* arena, uint64_t v, size_t* out_len) {
    char tmp[32];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (int)(v % 10));
            v /= 10;
        }
    }
    uint16_t* buf = (uint16_t*)ctsc_arena_alloc_aligned(arena, (size_t)n * sizeof(uint16_t), sizeof(uint16_t));
    for (int i = 0; i < n; i++) {
        buf[i] = (uint16_t)(uint8_t)tmp[n - 1 - i];
    }
    *out_len = (size_t)n;
    return buf;
}

/* Mirrors upstream/TypeScript/src/compiler/scanner.ts scanNumber (~1233). */
static void scan_number(CtscScanner* s) {
    size_t start = s->pos;

    if (s->source.data[s->pos] == '0') {
        s->pos++;
        size_t after_leading_zero = s->pos;
        bool all_octal = true;
        while (s->pos < s->source.len && is_digit(s->source.data[s->pos])) {
            if (!is_octal_digit(s->source.data[s->pos])) { all_octal = false; }
            s->pos++;
        }
        size_t extra_digits = s->pos - after_leading_zero;

        if (!all_octal) {
            /* NonOctalDecimalIntegerLiteral (ContainsLeadingZero).
             * Upstream scanner.ts ~1249/1308: tokenValue becomes the coerced
             * numeric form; for integer parts that is the decimal string with
             * leading zeros stripped. */
            (void)scan_number_fraction_and_exponent(s);
            if (s->diagnostics) {
                ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1489, (int)start, (int)(s->pos - start),
                    "Decimals with leading zeros are not allowed.");
            }
            s->current.kind = CTSC_SK_NumericLiteral;
            s->current.text = s->source.data + start;
            s->current.text_len = s->pos - start;
            /* TokenFlags.ContainsLeadingZero -> IsInvalid; emitter must use
             * the canonical tokenValue rather than the on-disk lexeme
             * (utilities.ts canUseOriginalText ~2036). */
            s->current.numeric_literal_is_invalid = true;
            set_numeric_coerced_value(s, start, s->pos);
            return;
        }
        if (extra_digits > 0) {
            /* LegacyOctalIntegerLiteral. tsc emits the DECIMAL value as
             * tokenValue (upstream scanner.ts ~1258:
             *   tokenValue = "" + parseInt(tokenValue, 8);
             * ), which is what parseLiteralLikeNode feeds into NumericLiteral
             * `text`. We do not consume a following '.' here (see `01.0`). */
            uint64_t v = 0;
            for (size_t i = after_leading_zero; i < s->pos; i++) {
                v = v * 8 + (uint64_t)(s->source.data[i] - '0');
            }
            size_t vlen;
            uint16_t* vbuf = u64_to_utf16_decimal((CtscArena*)s->arena_ptr, v, &vlen);
            if (s->diagnostics) {
                ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1121, (int)start, (int)(s->pos - start),
                    "Octal literals are not allowed. Use the syntax '{0}'.");
            }
            s->current.kind = CTSC_SK_NumericLiteral;
            s->current.text = s->source.data + start;
            s->current.text_len = s->pos - start;
            s->current.value = vbuf;
            s->current.value_len = vlen;
            /* TokenFlags.Octal -> IsInvalid; emitter must use the canonical
             * tokenValue rather than the on-disk lexeme (utilities.ts
             * canUseOriginalText ~2036). */
            s->current.numeric_literal_is_invalid = true;
            return;
        }
        /* Lone '0' before optional fraction/exponent. */
        bool coerce0 = scan_number_fraction_and_exponent(s);
        s->current.kind = CTSC_SK_NumericLiteral;
        s->current.text = s->source.data + start;
        s->current.text_len = s->pos - start;
        if (coerce0) { set_numeric_coerced_value(s, start, s->pos); }
        return;
    }

    while (s->pos < s->source.len && is_digit(s->source.data[s->pos])) { s->pos++; }
    bool coerce = scan_number_fraction_and_exponent(s);
    s->current.kind = CTSC_SK_NumericLiteral;
    s->current.text = s->source.data + start;
    s->current.text_len = s->pos - start;
    if (coerce) { set_numeric_coerced_value(s, start, s->pos); }
}

static void scan_string(CtscScanner* s) {
    uint16_t quote = s->source.data[s->pos];
    size_t start = s->pos;
    s->pos++;
    /* decoded value buffer */
    CtscArena* arena = (CtscArena*)s->arena_ptr;
    size_t cap = 16;
    uint16_t* buf = (uint16_t*)ctsc_arena_alloc_aligned(arena, cap * sizeof(uint16_t), sizeof(uint16_t));
    size_t len = 0;
    while (s->pos < s->source.len) {
        uint16_t c = s->source.data[s->pos];
        if (c == quote) { s->pos++; break; }
        if (is_line_break(c)) {
            if (s->diagnostics) {
                ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1002, (int)start, (int)(s->pos - start), "Unterminated string literal.");
            }
            break;
        }
        uint16_t out_c;
        if (c == '\\' && s->pos + 1 < s->source.len) {
            uint16_t e = s->source.data[s->pos + 1];
            s->pos += 2;
            switch (e) {
                case 'n':  out_c = 0x0A; break;
                case 'r':  out_c = 0x0D; break;
                case 't':  out_c = 0x09; break;
                case 'b':  out_c = 0x08; break;
                case 'f':  out_c = 0x0C; break;
                case 'v':  out_c = 0x0B; break;
                case '0':  out_c = 0x00; break;
                case '\\': out_c = 0x5C; break;
                case '"':  out_c = 0x22; break;
                case '\'': out_c = 0x27; break;
                case '`':  out_c = 0x60; break;
                default:   out_c = e; break;
            }
        } else {
            out_c = c;
            s->pos++;
        }
        if (len + 1 > cap) {
            size_t ncap = cap * 2;
            uint16_t* nb = (uint16_t*)ctsc_arena_alloc_aligned(arena, ncap * sizeof(uint16_t), sizeof(uint16_t));
            memcpy(nb, buf, len * sizeof(uint16_t));
            buf = nb;
            cap = ncap;
        }
        buf[len++] = out_c;
    }
    s->current.kind = CTSC_SK_StringLiteral;
    s->current.text = s->source.data + start;
    s->current.text_len = s->pos - start;
    s->current.value = buf;
    s->current.value_len = len;
}

static void value_ensure_cap(CtscScanner* s, uint16_t** buf, size_t len, size_t* cap, size_t need) {
    if (len + need <= *cap) { return; }
    CtscArena* arena = (CtscArena*)s->arena_ptr;
    size_t ncap = *cap ? *cap * 2 : 16;
    while (len + need > ncap) { ncap *= 2; }
    uint16_t* nb = (uint16_t*)ctsc_arena_alloc_aligned(arena, ncap * sizeof(uint16_t), sizeof(uint16_t));
    if (*buf && len) { memcpy(nb, *buf, len * sizeof(uint16_t)); }
    *buf = nb;
    *cap = ncap;
}

static void value_push_u16(CtscScanner* s, uint16_t** buf, size_t* len, size_t* cap, uint16_t c) {
    value_ensure_cap(s, buf, *len, cap, 1);
    (*buf)[(*len)++] = c;
}

static void value_append_utf16_range(CtscScanner* s, uint16_t** buf, size_t* len, size_t* cap,
    const uint16_t* data, size_t n) {
    if (n == 0) { return; }
    value_ensure_cap(s, buf, *len, cap, n);
    memcpy(*buf + *len, data, n * sizeof(uint16_t));
    *len += n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts scanEscapeSequence (~1543)
 * for template/string contexts (EscapeSequenceScanningFlags.String).
 * Pre: s->pos points at '\\'. Post: s->pos is after the escape sequence; decoded
 * units are appended to *buf / *len.
 */
static void scan_escape_sequence_string(CtscScanner* s, uint16_t** buf, size_t* len, size_t* cap,
    bool report_invalid_escape) {
    size_t esc_start = s->pos;
    (void)report_invalid_escape;
    s->pos++;
    if (s->pos >= s->source.len) {
        if (s->diagnostics) {
            ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1010, (int)esc_start, (int)(s->pos - esc_start),
                "Unexpected end of text.");
        }
        return;
    }
    uint16_t ch = s->source.data[s->pos++];
    switch (ch) {
        case 'n':  value_push_u16(s, buf, len, cap, 0x0A); break;
        case 'r':  value_push_u16(s, buf, len, cap, 0x0D); break;
        case 't':  value_push_u16(s, buf, len, cap, 0x09); break;
        case 'v':  value_push_u16(s, buf, len, cap, 0x0B); break;
        case 'b':  value_push_u16(s, buf, len, cap, 0x08); break;
        case 'f':  value_push_u16(s, buf, len, cap, 0x0C); break;
        case '\\': value_push_u16(s, buf, len, cap, 0x5C); break;
        case '"':  value_push_u16(s, buf, len, cap, 0x22); break;
        case '\'': value_push_u16(s, buf, len, cap, 0x27); break;
        case '`':  value_push_u16(s, buf, len, cap, 0x60); break;
        case '0':
            if (s->pos >= s->source.len || !is_digit(s->source.data[s->pos])) {
                value_push_u16(s, buf, len, cap, 0x00);
                break;
            }
            /* Legacy octal: 0-3 with up to 2 more octal digits, or 4-7 with one more. */
            {
                size_t opos = s->pos - 1;
                if (is_octal_digit(s->source.data[s->pos])) { s->pos++; }
                if (ch <= '3' && s->pos < s->source.len && is_octal_digit(s->source.data[s->pos])) {
                    s->pos++;
                }
                int code = 0;
                for (size_t i = opos; i < s->pos && i < opos + 3; i++) {
                    code = code * 8 + (int)(s->source.data[i] - '0');
                }
                value_push_u16(s, buf, len, cap, (uint16_t)code);
            }
            break;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            size_t opos = s->pos - 1;
            if (s->pos < s->source.len && is_octal_digit(s->source.data[s->pos])) { s->pos++; }
            if (ch <= '3' && s->pos < s->source.len && is_octal_digit(s->source.data[s->pos])) { s->pos++; }
            int code = 0;
            for (size_t i = opos; i < s->pos && i < opos + 3; i++) {
                code = code * 8 + (int)(s->source.data[i] - '0');
            }
            value_push_u16(s, buf, len, cap, (uint16_t)code);
            break;
        }
        case '8': case '9':
            value_push_u16(s, buf, len, cap, ch);
            break;
        case 'x': {
            int v = 0;
            for (int k = 0; k < 2; k++) {
                if (s->pos >= s->source.len || !is_hex_digit(s->source.data[s->pos])) {
                    if (s->diagnostics) {
                        ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1700, (int)esc_start,
                            (int)(s->pos - esc_start), "Hexadecimal digit expected.");
                    }
                    return;
                }
                v = (v << 4) | hex_digit_value(s->source.data[s->pos++]);
            }
            value_push_u16(s, buf, len, cap, (uint16_t)v);
            break;
        }
        case 'u': {
            if (s->pos < s->source.len && s->source.data[s->pos] == '{') {
                s->pos++;
                int v = -1;
                if (s->pos < s->source.len && is_hex_digit(s->source.data[s->pos])) {
                    v = 0;
                    while (s->pos < s->source.len && is_hex_digit(s->source.data[s->pos])) {
                        v = (v << 4) | hex_digit_value(s->source.data[s->pos++]);
                    }
                }
                if (s->pos >= s->source.len || s->source.data[s->pos] != '}') {
                    if (s->diagnostics) {
                        ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1701, (int)esc_start,
                            (int)(s->pos - esc_start), "Unterminated Unicode escape sequence.");
                    }
                    return;
                }
                s->pos++;
                if (v < 0 || v > 0x10FFFF) {
                    if (s->diagnostics) {
                        ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1702, (int)esc_start,
                            (int)(s->pos - esc_start),
                            "An extended Unicode escape value must be between 0x0 and 0x10FFFF inclusive.");
                    }
                    return;
                }
                if (v <= 0xFFFF) {
                    value_push_u16(s, buf, len, cap, (uint16_t)v);
                } else {
                    v -= 0x10000;
                    value_push_u16(s, buf, len, cap, (uint16_t)(0xD800 + (v >> 10)));
                    value_push_u16(s, buf, len, cap, (uint16_t)(0xDC00 + (v & 0x3FF)));
                }
                break;
            }
            {
                int v = 0;
                for (int k = 0; k < 4; k++) {
                    if (s->pos >= s->source.len || !is_hex_digit(s->source.data[s->pos])) {
                        if (s->diagnostics) {
                            ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1700, (int)esc_start,
                                (int)(s->pos - esc_start), "Hexadecimal digit expected.");
                        }
                        return;
                    }
                    v = (v << 4) | hex_digit_value(s->source.data[s->pos++]);
                }
                value_push_u16(s, buf, len, cap, (uint16_t)v);
            }
            break;
        }
        case 0x0D:
            if (s->pos < s->source.len && s->source.data[s->pos] == 0x0A) { s->pos++; }
            break;
        case 0x0A:
        case 0x2028:
        case 0x2029:
            break;
        default:
            value_push_u16(s, buf, len, cap, ch);
            break;
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts scanTemplateAndSetTokenValue (~1462).
 */
static void scan_template(CtscScanner* s) {
    bool started_with_backtick = (s->pos < s->source.len && s->source.data[s->pos] == '`');
    size_t token_start = s->pos;
    s->pos++;
    size_t content_start = s->pos;
    CtscArena* arena = (CtscArena*)s->arena_ptr;
    size_t cap = 16;
    uint16_t* buf = (uint16_t*)ctsc_arena_alloc_aligned(arena, cap * sizeof(uint16_t), sizeof(uint16_t));
    size_t vlen = 0;

    CtscSyntaxKind resulting = CTSC_SK_Unknown;

    for (;;) {
        if (s->pos >= s->source.len) {
            value_append_utf16_range(s, &buf, &vlen, &cap, s->source.data + content_start, s->pos - content_start);
            if (s->diagnostics) {
                ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1160, (int)token_start,
                    (int)(s->pos - token_start), "Unterminated template literal.");
            }
            resulting = started_with_backtick ? CTSC_SK_NoSubstitutionTemplateLiteral : CTSC_SK_TemplateTail;
            break;
        }

        uint16_t curr = s->source.data[s->pos];

        if (curr == '`') {
            value_append_utf16_range(s, &buf, &vlen, &cap, s->source.data + content_start, s->pos - content_start);
            s->pos++;
            resulting = started_with_backtick ? CTSC_SK_NoSubstitutionTemplateLiteral : CTSC_SK_TemplateTail;
            break;
        }

        if (curr == '$' && s->pos + 1 < s->source.len && s->source.data[s->pos + 1] == '{') {
            value_append_utf16_range(s, &buf, &vlen, &cap, s->source.data + content_start, s->pos - content_start);
            s->pos += 2;
            resulting = started_with_backtick ? CTSC_SK_TemplateHead : CTSC_SK_TemplateMiddle;
            break;
        }

        if (curr == '\\') {
            value_append_utf16_range(s, &buf, &vlen, &cap, s->source.data + content_start, s->pos - content_start);
            scan_escape_sequence_string(s, &buf, &vlen, &cap, /*report_invalid_escape*/ false);
            content_start = s->pos;
            continue;
        }

        if (curr == 0x0D) {
            value_append_utf16_range(s, &buf, &vlen, &cap, s->source.data + content_start, s->pos - content_start);
            s->pos++;
            if (s->pos < s->source.len && s->source.data[s->pos] == 0x0A) { s->pos++; }
            value_push_u16(s, &buf, &vlen, &cap, 0x0A);
            content_start = s->pos;
            continue;
        }

        s->pos++;
    }

    s->current.kind = resulting;
    s->current.text = s->source.data + token_start;
    s->current.text_len = s->pos - token_start;
    s->current.value = buf;
    s->current.value_len = vlen;
}

CtscSyntaxKind ctsc_scanner_next(CtscScanner* s) {
    s->current.full_start = (int)s->pos;
    skip_trivia(s);
    s->current.has_preceding_line_break = s->precedingLineBreak;
    s->current.text = NULL;
    s->current.text_len = 0;
    s->current.value = NULL;
    s->current.value_len = 0;
    s->current.numeric_literal_is_invalid = false;
    s->current.start = (int)s->pos;

    if (s->pos >= s->source.len) {
        s->current.kind = CTSC_SK_EndOfFileToken;
        s->current.end = (int)s->pos;
        return s->current.kind;
    }

    uint16_t c = s->source.data[s->pos];

    if (is_identifier_start(c)) { scan_identifier(s); goto done; }
    if (is_digit(c))            { scan_number(s);     goto done; }
    if (c == '"' || c == '\'')  { scan_string(s);     goto done; }
    if (c == '`')               { scan_template(s);   goto done; }

    switch (c) {
        case '{': s->pos++; s->current.kind = CTSC_SK_OpenBraceToken; break;
        case '}': s->pos++; s->current.kind = CTSC_SK_CloseBraceToken; break;
        case '(': s->pos++; s->current.kind = CTSC_SK_OpenParenToken; break;
        case ')': s->pos++; s->current.kind = CTSC_SK_CloseParenToken; break;
        case '[': s->pos++; s->current.kind = CTSC_SK_OpenBracketToken; break;
        case ']': s->pos++; s->current.kind = CTSC_SK_CloseBracketToken; break;
        case ';': s->pos++; s->current.kind = CTSC_SK_SemicolonToken; break;
        case ',': s->pos++; s->current.kind = CTSC_SK_CommaToken; break;
        case ':': s->pos++; s->current.kind = CTSC_SK_ColonToken; break;
        case '~': s->pos++; s->current.kind = CTSC_SK_TildeToken; break;
        case '@': s->pos++; s->current.kind = CTSC_SK_AtToken; break;
        case '.':
            if (peek(s, 1) == '.' && peek(s, 2) == '.') { s->pos += 3; s->current.kind = CTSC_SK_DotDotDotToken; }
            else if (is_digit(peek(s, 1))) { scan_number_leading_dot(s); }
            else { s->pos++; s->current.kind = CTSC_SK_DotToken; }
            break;
        case '?':
            if (peek(s, 1) == '?') { s->pos += 2; s->current.kind = CTSC_SK_QuestionQuestionToken; }
            else if (peek(s, 1) == '.' && !is_digit(peek(s, 2))) { s->pos += 2; s->current.kind = CTSC_SK_QuestionDotToken; }
            else { s->pos++; s->current.kind = CTSC_SK_QuestionToken; }
            break;
        case '=':
            if (peek(s, 1) == '=' && peek(s, 2) == '=') { s->pos += 3; s->current.kind = CTSC_SK_EqualsEqualsEqualsToken; }
            else if (peek(s, 1) == '=') { s->pos += 2; s->current.kind = CTSC_SK_EqualsEqualsToken; }
            else if (peek(s, 1) == '>') { s->pos += 2; s->current.kind = CTSC_SK_EqualsGreaterThanToken; }
            else { s->pos++; s->current.kind = CTSC_SK_EqualsToken; }
            break;
        case '!':
            if (peek(s, 1) == '=' && peek(s, 2) == '=') { s->pos += 3; s->current.kind = CTSC_SK_ExclamationEqualsEqualsToken; }
            else if (peek(s, 1) == '=') { s->pos += 2; s->current.kind = CTSC_SK_ExclamationEqualsToken; }
            else { s->pos++; s->current.kind = CTSC_SK_ExclamationToken; }
            break;
        case '<':
            if (peek(s, 1) == '=') { s->pos += 2; s->current.kind = CTSC_SK_LessThanEqualsToken; }
            else if (peek(s, 1) == '<') { s->pos += 2; s->current.kind = CTSC_SK_LessThanLessThanToken; }
            else { s->pos++; s->current.kind = CTSC_SK_LessThanToken; }
            break;
        case '>':
            /* tsc's scanner intentionally emits a single '>' here and lets the
             * parser re-scan via reScanGreaterToken() for '>=', '>>', '>>>',
             * '>>=' and '>>>=' (so generics like Array<Array<T>> parse). We
             * mirror that behaviour for byte-equal parity. See upstream
             * TypeScript src/compiler/scanner.ts :: scan() case '>'. */
            s->pos++;
            s->current.kind = CTSC_SK_GreaterThanToken;
            break;
        case '+':
            if (peek(s, 1) == '+') { s->pos += 2; s->current.kind = CTSC_SK_PlusPlusToken; }
            else if (peek(s, 1) == '=') { s->pos += 2; s->current.kind = CTSC_SK_PlusEqualsToken; }
            else { s->pos++; s->current.kind = CTSC_SK_PlusToken; }
            break;
        case '-':
            if (peek(s, 1) == '-') { s->pos += 2; s->current.kind = CTSC_SK_MinusMinusToken; }
            else if (peek(s, 1) == '=') { s->pos += 2; s->current.kind = CTSC_SK_MinusEqualsToken; }
            else { s->pos++; s->current.kind = CTSC_SK_MinusToken; }
            break;
        case '*':
            if (peek(s, 1) == '*') { s->pos += 2; s->current.kind = CTSC_SK_AsteriskAsteriskToken; }
            else if (peek(s, 1) == '=') { s->pos += 2; s->current.kind = CTSC_SK_AsteriskEqualsToken; }
            else { s->pos++; s->current.kind = CTSC_SK_AsteriskToken; }
            break;
        case '/':
            if (peek(s, 1) == '=') { s->pos += 2; s->current.kind = CTSC_SK_SlashEqualsToken; }
            else { s->pos++; s->current.kind = CTSC_SK_SlashToken; }
            break;
        case '%':
            if (peek(s, 1) == '=') { s->pos += 2; s->current.kind = CTSC_SK_PercentEqualsToken; }
            else { s->pos++; s->current.kind = CTSC_SK_PercentToken; }
            break;
        case '&':
            if (peek(s, 1) == '&') { s->pos += 2; s->current.kind = CTSC_SK_AmpersandAmpersandToken; }
            else { s->pos++; s->current.kind = CTSC_SK_AmpersandToken; }
            break;
        case '|':
            if (peek(s, 1) == '|') { s->pos += 2; s->current.kind = CTSC_SK_BarBarToken; }
            else { s->pos++; s->current.kind = CTSC_SK_BarToken; }
            break;
        case '^': s->pos++; s->current.kind = CTSC_SK_CaretToken; break;
        case '\\':
            /* An identifier may begin with a `\uXXXX` or `\u{XXXX}` escape
             * whose decoded code point is an IdentifierStart. See upstream
             * scanner.ts scan() case CharacterCodes.backslash (~2306). */
            if (try_scan_identifier_from_backslash(s)) { break; }
            if (s->diagnostics) {
                ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1127, (int)s->pos, 1, "Invalid character.");
            }
            s->pos++;
            s->current.kind = CTSC_SK_Unknown;
            break;
        default:
            if (s->diagnostics) {
                ctsc_diag_push(s->diagnostics, CTSC_DIAG_ERROR, 1127, (int)s->pos, 1, "Invalid character.");
            }
            s->pos++;
            s->current.kind = CTSC_SK_Unknown;
            break;
    }

done:
    s->current.end = (int)s->pos;
    return s->current.kind;
}

void ctsc_scanner_dump_tokens_json(const char* src, size_t len, CtscBuffer* out, bool pretty) {
    CtscArena arena; ctsc_arena_init(&arena, 64 * 1024);
    CtscDiagnosticList diags; ctsc_diag_init(&diags);
    CtscScanner sc; ctsc_scanner_init(&sc, src, len, &arena, &diags);

    CtscJson j; ctsc_json_init(&j, out, pretty);
    ctsc_json_begin_obj(&j);
    ctsc_json_key(&j, "tokens");
    ctsc_json_begin_arr(&j);
    while (ctsc_scanner_next(&sc) != CTSC_SK_EndOfFileToken) {
        ctsc_json_begin_obj(&j);
        ctsc_json_key(&j, "kind");  ctsc_json_cstr(&j, ctsc_syntax_kind_name(sc.current.kind));
        ctsc_json_key(&j, "start"); ctsc_json_int(&j, sc.current.start);
        ctsc_json_key(&j, "end");   ctsc_json_int(&j, sc.current.end);
        /* tsc's token JSON omits text/value for TemplateHead/Middle/Tail (substitution
         * fragments); see upstream scanner token reporting vs NoSubstitutionTemplateLiteral. */
        bool template_subst_fragment = sc.current.kind == CTSC_SK_TemplateHead
                || sc.current.kind == CTSC_SK_TemplateMiddle
                || sc.current.kind == CTSC_SK_TemplateTail;
        if (sc.current.text && sc.current.text_len && !template_subst_fragment) {
            ctsc_json_key(&j, "text");
            ctsc_json_str_utf16(&j, sc.current.text, sc.current.text_len);
        }
        if (sc.current.value && (sc.current.kind == CTSC_SK_StringLiteral
                || sc.current.kind == CTSC_SK_NoSubstitutionTemplateLiteral)) {
            ctsc_json_key(&j, "value");
            ctsc_json_str_utf16(&j, sc.current.value, sc.current.value_len);
        }
        ctsc_json_end_obj(&j);
    }
    /* Emit EOF marker */
    ctsc_json_begin_obj(&j);
    ctsc_json_key(&j, "kind");  ctsc_json_cstr(&j, "EndOfFileToken");
    ctsc_json_key(&j, "start"); ctsc_json_int(&j, sc.current.start);
    ctsc_json_key(&j, "end");   ctsc_json_int(&j, sc.current.end);
    ctsc_json_end_obj(&j);

    ctsc_json_end_arr(&j);
    ctsc_json_key(&j, "diagnostics");
    ctsc_json_begin_arr(&j);
    for (CtscDiagnostic* d = diags.head; d; d = d->next) {
        ctsc_json_begin_obj(&j);
        ctsc_json_key(&j, "code");     ctsc_json_int(&j, d->code);
        ctsc_json_key(&j, "start");    ctsc_json_int(&j, d->start);
        ctsc_json_key(&j, "length");   ctsc_json_int(&j, d->length);
        ctsc_json_key(&j, "category"); ctsc_json_int(&j, (long long)d->category);
        ctsc_json_key(&j, "message");  ctsc_json_cstr(&j, d->message);
        ctsc_json_end_obj(&j);
    }
    ctsc_json_end_arr(&j);
    ctsc_json_end_obj(&j);

    ctsc_scanner_free(&sc);
    ctsc_diag_free(&diags);
    ctsc_arena_free(&arena);
}
