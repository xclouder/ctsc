#include "ctsc/emitter.h"
#include "ctsc/ast.h"
#include "ctsc/scanner.h"
#include "ctsc/buffer.h"
#include "ctsc/utf8.h"

#include <stdint.h>
#include <string.h>

/*
 * Mirrors upstream/TypeScript/src/compiler/utilities.ts createTextWriter
 * (~6298): the printer keeps track of whether it is at the start of a line
 * and, when it is, prepends the current indent string to the next chunk of
 * text it writes (writeText ~6322). Consecutive `writer.writeLine()` calls
 * collapse into a single newline (writeLine ~6366: `if (!lineStart || force)`).
 *
 * Preserving that state lets the BinaryExpression emitter (createEmitBinary-
 * Expression ~2839) call `writeLinesAndIndent` (~4987) from both the inner
 * and outer ends of a cascade without duplicating line breaks — which is
 * exactly what `1 >\n> 2;` (parserGreaterThanTokenAmbiguity4) exercises.
 */
typedef struct {
    CtscBuffer*         out;
    const CtscUtf16Buf* source; /* nullable */
    int                 indent;
    bool                line_start;
} Emitter;

static void write_indent_if_needed(Emitter* e) {
    if (!e->line_start) return;
    e->line_start = false;
    for (int i = 0; i < e->indent; ++i) ctsc_buf_append_cstr(e->out, "    ");
}

static void write_char(Emitter* e, char c) {
    write_indent_if_needed(e);
    ctsc_buf_append_char(e->out, c);
}

static void write_cstr(Emitter* e, const char* s) {
    if (!*s) return;
    write_indent_if_needed(e);
    ctsc_buf_append_cstr(e->out, s);
}

static void write_u16(Emitter* e, const uint16_t* data, size_t len) {
    if (len == 0) return;
    write_indent_if_needed(e);
    char buf[8];
    for (size_t i = 0; i < len; ++i) {
        uint16_t u = data[i];
        if (u < 0x80) { buf[0] = (char)u; ctsc_buf_append(e->out, buf, 1); }
        else if (u < 0x800) {
            buf[0] = (char)(0xC0 | (u >> 6));
            buf[1] = (char)(0x80 | (u & 0x3F));
            ctsc_buf_append(e->out, buf, 2);
        } else {
            buf[0] = (char)(0xE0 | (u >> 12));
            buf[1] = (char)(0x80 | ((u >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (u & 0x3F));
            ctsc_buf_append(e->out, buf, 3);
        }
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/utilities.ts escapeString (~6207)
 * with doubleQuoteEscapedCharsRegExp / singleQuoteEscapedCharsRegExp (~6160-6161),
 * getReplacement (~6187), and encodeUtf16EscapeSequence (~6181).
 */
static void write_u16_hex_escape(Emitter* e, uint16_t u) {
    static const char hexdigits[] = "0123456789ABCDEF";
    write_indent_if_needed(e);
    char buf[6];
    buf[0] = '\\';
    buf[1] = 'u';
    buf[2] = hexdigits[(u >> 12) & 0xF];
    buf[3] = hexdigits[(u >> 8) & 0xF];
    buf[4] = hexdigits[(u >> 4) & 0xF];
    buf[5] = hexdigits[u & 0xF];
    ctsc_buf_append(e->out, buf, 6);
}

static void write_string_literal_escaped(Emitter* e, const uint16_t* data, size_t len, bool single_quote) {
    for (size_t i = 0; i < len; ++i) {
        uint16_t u = data[i];
        if (u == '\\') {
            write_cstr(e, "\\\\");
        } else if ((!single_quote && u == '"') || (single_quote && u == '\'')) {
            write_cstr(e, single_quote ? "\\'" : "\\\"");
        } else if (u == 0) {
            bool next_is_digit = (i + 1 < len && data[i + 1] >= (uint16_t)'0' && data[i + 1] <= (uint16_t)'9');
            write_cstr(e, next_is_digit ? "\\x00" : "\\0");
        } else if (u == 0x08) {
            write_cstr(e, "\\b");
        } else if (u == 0x09) {
            write_cstr(e, "\\t");
        } else if (u == 0x0A) {
            write_cstr(e, "\\n");
        } else if (u == 0x0B) {
            write_cstr(e, "\\v");
        } else if (u == 0x0C) {
            write_cstr(e, "\\f");
        } else if (u == 0x0D) {
            write_cstr(e, "\\r");
        } else if (u <= 0x1F) {
            write_u16_hex_escape(e, u);
        } else if (u == 0x2028) {
            write_cstr(e, "\\u2028");
        } else if (u == 0x2029) {
            write_cstr(e, "\\u2029");
        } else if (u == 0x0085) {
            write_cstr(e, "\\u0085");
        } else {
            write_u16(e, &data[i], 1);
        }
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts writeLine (~4919) which
 * delegates to the text writer's writeLine (utilities.ts ~6366): the newline
 * is only appended when we are not already at a line start, so back-to-back
 * line breaks collapse into one. `force` (the `i > 0` argument in the loop)
 * is only relevant for count > 1 which we do not currently need.
 */
static void write_line(Emitter* e) {
    if (e->line_start) return;
    ctsc_buf_append_char(e->out, '\n');
    e->line_start = true;
}

/*
 * Emit a raw newline that does NOT collapse with a previous writeLine — used
 * for cases where tsc writes newlines through `write("\n")` (which goes
 * through writeText and resets lineStart via updateLineCountAndPosFor,
 * utilities.ts ~6310). Keeps the SourceFile statement separator and the
 * leading-comments replay from losing a newline when the comment or the
 * previous statement ended at a line start.
 */
static void write_raw_newline(Emitter* e) {
    ctsc_buf_append_char(e->out, '\n');
    e->line_start = true;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts writeLinesAndIndent
 * (~4987): when the computed line count between two sibling positions is
 * positive, increase indent and write the newlines; otherwise optionally
 * write a single separating space.
 */
static void write_lines_and_indent(Emitter* e, int line_count, bool write_space_if_not_indenting) {
    if (line_count > 0) {
        e->indent++;
        write_line(e);
    } else if (write_space_if_not_indenting) {
        write_char(e, ' ');
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts decreaseIndentIf (~5001):
 * drop one indent level per truthy argument.
 */
static void decrease_indent_if(Emitter* e, int value1, int value2) {
    if (value1) e->indent--;
    if (value2) e->indent--;
}

static const char* op_text(CtscSyntaxKind k) {
    switch (k) {
        case CTSC_SK_PlusToken:                                   return "+";
        case CTSC_SK_MinusToken:                                  return "-";
        case CTSC_SK_AsteriskToken:                               return "*";
        case CTSC_SK_AsteriskAsteriskToken:                       return "**";
        case CTSC_SK_SlashToken:                                  return "/";
        case CTSC_SK_PercentToken:                                return "%";
        case CTSC_SK_PlusPlusToken:                               return "++";
        case CTSC_SK_MinusMinusToken:                             return "--";
        case CTSC_SK_ExclamationToken:                            return "!";
        case CTSC_SK_TildeToken:                                  return "~";
        case CTSC_SK_EqualsEqualsToken:                           return "==";
        case CTSC_SK_ExclamationEqualsToken:                      return "!=";
        case CTSC_SK_EqualsEqualsEqualsToken:                     return "===";
        case CTSC_SK_ExclamationEqualsEqualsToken:                return "!==";
        case CTSC_SK_LessThanToken:                               return "<";
        case CTSC_SK_GreaterThanToken:                            return ">";
        case CTSC_SK_LessThanEqualsToken:                         return "<=";
        case CTSC_SK_GreaterThanEqualsToken:                      return ">=";
        case CTSC_SK_InstanceOfKeyword:                           return "instanceof";
        case CTSC_SK_InKeyword:                                   return "in";
        case CTSC_SK_LessThanLessThanToken:                       return "<<";
        case CTSC_SK_GreaterThanGreaterThanToken:                 return ">>";
        case CTSC_SK_GreaterThanGreaterThanGreaterThanToken:      return ">>>";
        case CTSC_SK_AmpersandToken:                              return "&";
        case CTSC_SK_BarToken:                                    return "|";
        case CTSC_SK_CaretToken:                                  return "^";
        case CTSC_SK_AmpersandAmpersandToken:                     return "&&";
        case CTSC_SK_BarBarToken:                                 return "||";
        case CTSC_SK_QuestionQuestionToken:                       return "??";
        case CTSC_SK_EqualsToken:                                 return "=";
        case CTSC_SK_PlusEqualsToken:                             return "+=";
        case CTSC_SK_MinusEqualsToken:                            return "-=";
        case CTSC_SK_AsteriskEqualsToken:                         return "*=";
        case CTSC_SK_SlashEqualsToken:                            return "/=";
        case CTSC_SK_PercentEqualsToken:                          return "%=";
        case CTSC_SK_GreaterThanGreaterThanEqualsToken:           return ">>=";
        case CTSC_SK_GreaterThanGreaterThanGreaterThanEqualsToken:return ">>>=";
        /* Mirrors upstream/TypeScript/src/compiler/scanner.ts tokenStrings
         * (~126) for CommaToken == ",". The comma operator forms a
         * BinaryExpression via parser.ts parseExpression (~5055), and the
         * emitter prints it through the shared createEmitBinaryExpression
         * path (~2839). Empty separator on the LHS, single space on the
         * RHS — the ` ` around the operator comes from write_lines_and_indent's
         * `writeSpaceIfNotIndenting`, but createEmitBinaryExpression
         * suppresses the LEADING space for CommaToken (onOperator ~2884:
         * `writeLinesAndIndent(..., isCommaOperator)`). We mirror that by
         * passing isCommaOperator into the BinaryExpression emit below. */
        case CTSC_SK_CommaToken:                                  return ",";
        default:                                                  return "?";
    }
}

static bool is_line_break_u16(uint16_t c) {
    return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029;
}

static bool is_ws_single_line_u16(uint16_t c) {
    return c == 0x09 || c == 0x0B || c == 0x0C || c == 0x20;
}

/*
 * Simplified port of upstream/TypeScript/src/compiler/scanner.ts skipTrivia
 * (~641). Enough to cover whitespace, line terminators, single-line `//` and
 * block slash-star comments — which is all that the binary-expression
 * emitter needs in order to compute line counts between sibling positions
 * via `getStartPositionOfRange` (utilities.ts ~7965).
 */
static size_t skip_trivia_u16(const CtscUtf16Buf* src, size_t pos) {
    if (!src) return pos;
    const uint16_t* t = src->data;
    size_t len = src->len;
    while (pos < len) {
        uint16_t c = t[pos];
        if (c == 0x0D) {
            if (pos + 1 < len && t[pos + 1] == 0x0A) pos += 2; else pos++;
            continue;
        }
        if (c == 0x0A || c == 0x2028 || c == 0x2029) { pos++; continue; }
        if (is_ws_single_line_u16(c)) { pos++; continue; }
        if (c == '/' && pos + 1 < len) {
            if (t[pos + 1] == '/') {
                pos += 2;
                while (pos < len && !is_line_break_u16(t[pos])) pos++;
                continue;
            }
            if (t[pos + 1] == '*') {
                pos += 2;
                while (pos + 1 < len && !(t[pos] == '*' && t[pos + 1] == '/')) pos++;
                if (pos + 1 < len) pos += 2;
                continue;
            }
        }
        break;
    }
    return pos;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts iterateCommentRanges
 * (~826) with trailing=true: scan forward from `pos` through spaces, tabs,
 * and form-feeds (NOT line breaks: a line break terminates the scan),
 * collecting double-slash and slash-star comments. Each comment is
 * delivered via the prefix-space convention of emitter.ts
 * emitTrailingComment (~6051): insert a single space before the first
 * comment when the writer is not at a line start, copy the comment body
 * verbatim, and, if the comment had a line break after it (only possible
 * for block comments, since single-line comments end AT the line break
 * and the trailing scan exits before emitting one), write a line via the
 * collapsing writer.writeLine.
 *
 * ctsc does not yet model the containerPos/containerEnd suppression
 * stack from pipelineEmitWithComments (~5795); we call this helper only
 * at the points where tsc emitTrailingComments(end) would actually fire
 * (parserGreaterThanTokenAmbiguity13 needs it at the end of the first
 * ExpressionStatement, whose end, the missing-identifier right operand
 * at pos 23, does not equal the SourceFile containerEnd).
 */
static void emit_trailing_comments_after(Emitter* e, size_t pos) {
    if (!e->source) return;
    const uint16_t* t = e->source->data;
    size_t len = e->source->len;
    while (pos < len) {
        uint16_t c = t[pos];
        if (c == 0x09 || c == 0x0B || c == 0x0C || c == 0x20) { pos++; continue; }
        if (c == '/' && pos + 1 < len && (t[pos + 1] == '/' || t[pos + 1] == '*')) {
            bool is_block = t[pos + 1] == '*';
            size_t start = pos;
            pos += 2;
            if (!is_block) {
                while (pos < len && !is_line_break_u16(t[pos])) pos++;
            } else {
                while (pos + 1 < len && !(t[pos] == '*' && t[pos + 1] == '/')) pos++;
                if (pos + 1 < len) pos += 2;
            }
            /* emitter.ts emitTrailingComment (~6051): space prefix when the
             * writer is not at start-of-line. Then the raw comment body. */
            if (!e->line_start) write_char(e, ' ');
            write_u16(e, t + start, pos - start);
            /* hasTrailingNewLine: only set for block comments when the
             * post-comment scan hits a line break before any other non-
             * whitespace char (scanner.ts iterateCommentRanges ~915).
             * Single-line comments don't reach here because the trailing
             * scan exits at the line break that terminates them. */
            if (is_block) {
                size_t scan = pos;
                bool has_nl = false;
                while (scan < len) {
                    uint16_t d = t[scan];
                    if (is_line_break_u16(d)) { has_nl = true; break; }
                    if (d == 0x09 || d == 0x0B || d == 0x0C || d == 0x20) { scan++; continue; }
                    break;
                }
                if (has_nl) write_line(e);
            }
            continue;
        }
        /* Line break terminates the trailing-comment scan (trailing=true
         * branch of iterateCommentRanges ~850). */
        break;
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts getLinesBetweenPositions
 * (~537): count the number of line terminators in [pos1, pos2). A CR+LF pair
 * counts as one logical line break. Order-insensitive like the upstream
 * function, but we only need the non-negative direction.
 */
static int lines_between_positions(const CtscUtf16Buf* src, size_t pos1, size_t pos2) {
    if (!src) return 0;
    if (pos1 == pos2) return 0;
    if (pos1 > pos2) { size_t tmp = pos1; pos1 = pos2; pos2 = tmp; }
    const uint16_t* t = src->data;
    size_t len = src->len;
    if (pos2 > len) pos2 = len;
    int lines = 0;
    for (size_t i = pos1; i < pos2; ) {
        uint16_t c = t[i];
        if (c == 0x0D) {
            lines++;
            if (i + 1 < pos2 && t[i + 1] == 0x0A) i += 2; else i++;
            continue;
        }
        if (c == 0x0A || c == 0x2028 || c == 0x2029) { lines++; i++; continue; }
        i++;
    }
    return lines;
}

#define CTSC_MAX_LEADING_COMMENTS 64

typedef struct {
    size_t pos;
    size_t end;
    bool is_block;
    bool has_trailing_nl;
} CtscCommentRangeRec;

/*
 * 0-based line index of `pos` (UTF-16 offset), mirroring
 * getLineOfLocalPositionFromLineMap (utilities.ts) for emitter use.
 */
static int line_of_position(const CtscUtf16Buf* src, size_t pos) {
    if (!src) return 0;
    const uint16_t* t = src->data;
    size_t len = src->len;
    if (pos > len) pos = len;
    int line = 0;
    for (size_t i = 0; i < pos;) {
        uint16_t c = t[i];
        if (c == 0x0D) {
            line++;
            if (i + 1 < pos && t[i + 1] == 0x0A) i += 2; else i++;
            continue;
        }
        if (c == 0x0A || c == 0x2028 || c == 0x2029) { line++; i++; continue; }
        i++;
    }
    return line;
}

static bool is_white_space_like_u16_emitter(uint16_t ch) {
    if (ch < 128) {
        return ch == 0x09 || ch == 0x0B || ch == 0x0C || ch == 0x20;
    }
    return ch == 0x00A0 || ch == 0x0085 || ch == 0x1680
        || (ch >= 0x2000 && ch <= 0x200B) || ch == 0x202F || ch == 0x205F
        || ch == 0x3000 || ch == 0xFEFF;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts iterateCommentRanges
 * (~826) in reduce mode: getLeadingCommentRanges (scanner.ts ~957).
 */
static size_t collect_leading_comment_ranges(
    const CtscUtf16Buf* src,
    size_t pos_in,
    CtscCommentRangeRec* out,
    size_t max_out) {
    if (!src || max_out == 0) return 0;
    const uint16_t* t = src->data;
    size_t len = src->len;
    size_t pos = pos_in;
    if (pos > len) return 0;

    bool collecting = false;
    if (pos == 0) {
        collecting = true;
        if (len >= 2 && t[0] == '#' && t[1] == '!') {
            while (pos < len && !is_line_break_u16(t[pos])) pos++;
        }
    }

    size_t out_count = 0;
    size_t pending_pos = 0;
    size_t pending_end = 0;
    bool pending_block = false;
    bool pending_has_trailing_nl = false;
    bool has_pending = false;

    while (pos < len) {
        uint16_t ch = t[pos];
        if (ch == 0x0D) {
            if (pos + 1 < len && t[pos + 1] == 0x0A) pos += 2; else pos++;
            collecting = true;
            if (has_pending) pending_has_trailing_nl = true;
            continue;
        }
        if (ch == 0x0A || ch == 0x2028 || ch == 0x2029) {
            pos++;
            collecting = true;
            if (has_pending) pending_has_trailing_nl = true;
            continue;
        }
        if (ch == 0x09 || ch == 0x0B || ch == 0x0C || ch == 0x20) { pos++; continue; }
        if (ch == 0xFEFF) { pos++; continue; }
        if (ch > 127 && is_white_space_like_u16_emitter(ch)) {
            pos++;
            continue;
        }
        if (ch == '/' && pos + 1 < len && (t[pos + 1] == '/' || t[pos + 1] == '*')) {
            bool is_block = t[pos + 1] == '*';
            size_t start_pos = pos;
            pos += 2;
            bool has_trailing_nl = false;
            if (!is_block) {
                while (pos < len && !is_line_break_u16(t[pos])) pos++;
                if (pos < len) has_trailing_nl = true;
            } else {
                while (pos + 1 < len && !(t[pos] == '*' && t[pos + 1] == '/')) pos++;
                if (pos + 1 < len) pos += 2;
                size_t scan = pos;
                while (scan < len) {
                    uint16_t d = t[scan];
                    if (is_line_break_u16(d)) { has_trailing_nl = true; break; }
                    if (d == 0x09 || d == 0x0B || d == 0x0C || d == 0x20) { scan++; continue; }
                    break;
                }
            }
            if (collecting) {
                if (has_pending) {
                    if (out_count < max_out) {
                        out[out_count].pos = pending_pos;
                        out[out_count].end = pending_end;
                        out[out_count].is_block = pending_block;
                        out[out_count].has_trailing_nl = pending_has_trailing_nl;
                        out_count++;
                    } else {
                        return out_count;
                    }
                }
                pending_pos = start_pos;
                pending_end = pos;
                pending_block = is_block;
                pending_has_trailing_nl = has_trailing_nl;
                has_pending = true;
            }
            continue;
        }
        break;
    }
    if (has_pending && out_count < max_out) {
        out[out_count].pos = pending_pos;
        out[out_count].end = pending_end;
        out[out_count].is_block = pending_block;
        out[out_count].has_trailing_nl = pending_has_trailing_nl;
        out_count++;
    }
    return out_count;
}

/*
 * Mirrors upstream utilities.ts emitDetachedComments (~7012) for the
 * SourceFile statements list: copyright-style comments separated by at least
 * one blank line from the first statement are emitted before emitList.
 * Selfhost-derived 91_union_type_alias_object.ts (leading `//` header before
 * elided `type` aliases).
 */
/*
 * When `emit_detached_comments` returns true, `*out_last_detached_comment_src_end`
 * is set to the UTF-16 index that `collect_leading_comment_ranges` stores as
 * `.end` on the last detached range (same as TS CommentRange.end for //-:
 * the first line-break code unit after the comment text). Callers resume
 * leading-comment emission from that index (emitter.ts ~6128).
 */
static bool emit_detached_comments(Emitter* e, size_t statements_node_pos, size_t* out_last_detached_comment_src_end) {
    if (!e->source) return false;
    const CtscUtf16Buf* src = e->source;
    CtscCommentRangeRec leading[CTSC_MAX_LEADING_COMMENTS];
    size_t n_leading = collect_leading_comment_ranges(
        src, statements_node_pos, leading, CTSC_MAX_LEADING_COMMENTS);
    if (n_leading == 0) return false;

    CtscCommentRangeRec detached[CTSC_MAX_LEADING_COMMENTS];
    size_t n_detached = 0;
    for (size_t i = 0; i < n_leading; i++) {
        if (n_detached > 0) {
            int last_line = line_of_position(src, detached[n_detached - 1].end);
            int comment_line = line_of_position(src, leading[i].pos);
            if (comment_line >= last_line + 2) break;
        }
        detached[n_detached++] = leading[i];
    }
    if (n_detached == 0) return false;

    size_t last_detached_end = detached[n_detached - 1].end;
    if (out_last_detached_comment_src_end) {
        *out_last_detached_comment_src_end = last_detached_end;
    }
    int last_comment_line = line_of_position(src, last_detached_end);
    size_t after_trivia = skip_trivia_u16(src, statements_node_pos);
    int node_line = line_of_position(src, after_trivia);
    if (node_line < last_comment_line + 2) return false;

    /* emitNewLineBeforeLeadingComments (utilities.ts ~6938) */
    if (statements_node_pos != detached[0].pos
        && line_of_position(src, statements_node_pos)
            != line_of_position(src, detached[0].pos)) {
        write_raw_newline(e);
    }

    /* emitComments (utilities.ts ~6963), leadingSeparator=false, trailingSeparator=true */
    bool emit_intervening_sep = false;
    for (size_t i = 0; i < n_detached; i++) {
        if (emit_intervening_sep) {
            write_char(e, ' ');
            emit_intervening_sep = false;
        }
        write_u16(e, src->data + detached[i].pos, detached[i].end - detached[i].pos);
        if (detached[i].has_trailing_nl) {
            write_raw_newline(e);
        } else {
            emit_intervening_sep = true;
        }
    }
    if (emit_intervening_sep) {
        write_char(e, ' ');
    }
    return true;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts getLinesBetweenNodes
 * (~5196): without preserveSourceNewlines (the transpileModule default) tsc
 * returns `rangeEndIsOnSameLineAsRangeStart(node1, node2) ? 0 : 1`, where
 * the start of range2 is taken through skipTrivia (utilities.ts ~7965).
 * That is sufficient for the cascade in 105_parserGreaterThanTokenAmbiguity4.
 */
static void emit(Emitter* e, const CtscNode* n);
static void emit_named_import_bindings_value_only(Emitter* e, const CtscNode* nb);
static void emit_import_clause_js(Emitter* e, const CtscNode* clause);
static bool block_function_body_should_emit_single_line(Emitter* e, const CtscNode* block);
static void emit_function_like_block(Emitter* e, const CtscNode* block, bool allow_single_line_compact);

/*
 * Mirrors upstream/TypeScript/src/compiler/transformers/ts.ts
 * visitFunctionDeclaration (~1555) → shouldEmitFunctionLikeDeclaration
 * (~1297): a top-level FunctionDeclaration with a missing body (overload
 * signature) is replaced with a NotEmittedStatement, which the printer
 * emits as zero bytes (emitter.ts ~2017). For the SourceFile loop this
 * means we must (a) skip the statement entirely, (b) skip the trailing
 * separator newline that follows it, and (c) skip the leading comments
 * tied to it — emitLeadingComments (~5976) with isEmittedNode=false at
 * pos=0 only forwards triple-slash comments (`/// ...`), so any normal
 * `//` or block comment dies with the dropped node.
 *
 * Also mirrors transformers/ts.ts visitTypeScript (~643): every statement
 * that carries ModifierFlags.Ambient (the `declare` modifier, set by
 * parser.ts parseDeclaration ~7474) is replaced with a NotEmittedStatement.
 * ctsc tracks that on the VariableStatement node via `has_declare`; when a
 * future fixture unlocks `declare function` / `declare class` / etc., mirror
 * the same flag on those statement data structs and extend this predicate.
 */
static bool named_import_bindings_has_value_specifier(const CtscNode* nb) {
    if (!nb) return false;
    if (nb->kind == CTSC_SK_NamespaceImport) return true;
    if (nb->kind != CTSC_SK_NamedImports) return false;
    const CtscNodeArray* el = &nb->data.namedImports.elements;
    for (size_t i = 0; i < el->len; i++) {
        CtscNode* sp = el->items[i];
        if (sp->kind == CTSC_SK_ImportSpecifier && !sp->data.importSpecifier.is_type_only) return true;
    }
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/transformers/ts.ts visitImportDeclaration
 * (~2259-2280): elide `import type ...` and imports whose clause has no value
 * bindings after stripping inline `type` specifiers (visitImportSpecifier ~2319).
 */
static bool source_file_import_declaration_is_elided(const CtscNode* s) {
    if (s->kind != CTSC_SK_ImportDeclaration) return false;
    const CtscNode* clause = s->data.importDeclaration.importClause;
    if (!clause) return false;
    if (clause->kind != CTSC_SK_ImportClause) return false;
    if (clause->data.importClause.is_type_only) return true;
    if (clause->data.importClause.name) return false;
    return !named_import_bindings_has_value_specifier(clause->data.importClause.namedBindings);
}

static bool source_file_statement_is_dropped(const CtscNode* s) {
    if (!s) return true;
    if (s->kind == CTSC_SK_FunctionDeclaration
        && s->data.functionDeclaration.body == NULL) return true;
    if (s->kind == CTSC_SK_FunctionDeclaration
        && s->data.functionDeclaration.has_declare) return true;
    if (s->kind == CTSC_SK_VariableStatement
        && s->data.variableStatement.has_declare) return true;
    /* Mirrors upstream/TypeScript/src/compiler/transformers/ts.ts
     * visitTypeScript (~649): InterfaceDeclaration is a type-only declaration
     * that the TS-to-JS transformer replaces with createNotEmittedStatement
     * (see visitTypeScript's InterfaceDeclaration case which returns undefined /
     * an elided node). The printer then drops the statement along with its
     * leading comments, matching `tsc`'s output for an interface-only source. */
    if (s->kind == CTSC_SK_InterfaceDeclaration) return true;
    /* Mirrors upstream transformers/ts.ts visitTypeScript (~643): TypeAliasDeclaration
     * is elided (createNotEmittedStatement); transpileModule emits no JS for it. */
    if (s->kind == CTSC_SK_TypeAliasDeclaration) return true;
    if (source_file_import_declaration_is_elided(s)) return true;
    /* Mirrors upstream transformers/ts.ts visitExportDeclaration (~2341):
     * `export type { ... } from` is elided entirely. */
    if (s->kind == CTSC_SK_ExportDeclaration && s->data.exportDeclaration.is_type_only) return true;
    return false;
}

/*
 * Mirrors upstream parser.ts isAnExternalModuleIndicatorNode (~476) /
 * isFileProbablyExternalModule (~469): any top-level import/export shape
 * means the SourceFile is an ES module (parser.ts externalModuleIndicator).
 */
static bool modifier_array_has_export_keyword(const CtscNodeArray* mods) {
    if (!mods) return false;
    for (size_t i = 0; i < mods->len; i++) {
        const CtscNode* m = mods->items[i];
        if (m && m->kind == CTSC_SK_ExportKeyword) return true;
    }
    return false;
}

static bool statement_indicates_source_file_is_external_module(const CtscNode* s) {
    if (!s) return false;
    switch (s->kind) {
        case CTSC_SK_ImportDeclaration:
        case CTSC_SK_ExportDeclaration:
            return true;
        case CTSC_SK_VariableStatement:
            return s->data.variableStatement.has_export;
        case CTSC_SK_FunctionDeclaration:
            return s->data.functionDeclaration.has_export || s->data.functionDeclaration.has_export_default;
        case CTSC_SK_ClassDeclaration:
            return s->data.classDeclaration.has_export;
        case CTSC_SK_EnumDeclaration:
            return s->data.enumDeclaration.has_export;
        case CTSC_SK_InterfaceDeclaration:
            return modifier_array_has_export_keyword(&s->data.classDeclaration.modifiers);
        case CTSC_SK_TypeAliasDeclaration:
            return modifier_array_has_export_keyword(&s->data.typeAliasDeclaration.modifiers);
        default:
            return false;
    }
}

static bool source_file_is_external_module(const CtscNode* sf) {
    if (!sf || sf->kind != CTSC_SK_SourceFile) return false;
    const CtscNodeArray* ss = &sf->data.sourceFile.statements;
    for (size_t i = 0; i < ss->len; i++) {
        if (statement_indicates_source_file_is_external_module(ss->items[i])) return true;
    }
    return false;
}

/*
 * Mirrors upstream utilitiesPublic.ts isExternalModuleIndicator (~2148) on
 * statements that survive TS-to-JS elision (transformers/module/esnextAnd2015.ts
 * ~114: some(result.statements, isExternalModuleIndicator)).
 */
static bool source_file_emitted_statement_is_external_module_indicator(const CtscNode* s) {
    if (!s || source_file_statement_is_dropped(s)) return false;
    switch (s->kind) {
        case CTSC_SK_ImportDeclaration:
            return !source_file_import_declaration_is_elided(s);
        case CTSC_SK_ExportDeclaration:
            return !s->data.exportDeclaration.is_type_only;
        case CTSC_SK_VariableStatement:
            return s->data.variableStatement.has_export;
        case CTSC_SK_FunctionDeclaration:
            return s->data.functionDeclaration.has_export || s->data.functionDeclaration.has_export_default;
        case CTSC_SK_ClassDeclaration:
            return s->data.classDeclaration.has_export;
        case CTSC_SK_EnumDeclaration:
            return s->data.enumDeclaration.has_export;
        default:
            return false;
    }
}

/*
 * createEmptyExports (factory/utilities.ts ~183) appended by
 * transformECMAScriptModule (esnextAnd2015.ts ~114-119) when the module
 * has no remaining import/export surface after type erasure.
 */
static bool source_file_needs_synthetic_empty_export(const CtscNode* sf, const CtscNodeArray* ss) {
    if (!source_file_is_external_module(sf)) return false;
    for (size_t i = 0; i < ss->len; i++) {
        if (source_file_emitted_statement_is_external_module_indicator(ss->items[i])) return false;
    }
    return true;
}

static void emit_list(Emitter* e, const CtscNodeArray* arr, const char* sep) {
    for (size_t i = 0; i < arr->len; ++i) {
        if (i) write_cstr(e, sep);
        emit(e, arr->items[i]);
    }
}

/*
 * Class instance field initialisers are lowered into the constructor by the
 * TS→JS transformer (transformers/ts.ts visitClassDeclaration / classFields)
 * before the printer runs; ctsc mirrors that in the emitter so transpileModule
 * output matches tsc (e.g. `value: number = 0` → `this.value = 0` in
 * `constructor()`).
 */
static bool identifier_text_equals_ascii(const CtscNode* id, const char* ascii) {
    if (!id || id->kind != CTSC_SK_Identifier) return false;
    const uint16_t* t = id->data.identifier.text;
    size_t len = id->data.identifier.text_len;
    for (size_t i = 0;; ++i) {
        unsigned char c = (unsigned char)ascii[i];
        if (c == 0) return i == len;
        if (i >= len || t[i] != (uint16_t)c) return false;
    }
}

/*
 * Mirrors upstream transformers/ts.ts visitParameter (~2069): a leading
 * ParameterDeclaration whose name is `this` is a type-only pseudo-parameter
 * and is omitted from the printed JS (emitter.ts emitParameterList still
 * walks parameters after the transformer; ctsc has no separate transform
 * pass). Selfhost-derived 74_export_generic_rest.ts:
 *     function (this: unknown, ...args) {}
 *     → function (...args) {}
 */
static bool parameter_is_this_typing_only(const CtscNode* p) {
    if (!p || p->kind != CTSC_SK_Parameter) return false;
    return identifier_text_equals_ascii(p->data.parameter.name, "this");
}

static void emit_parameter_list_for_js(Emitter* e, const CtscNodeArray* params) {
    bool first = true;
    for (size_t i = 0; i < params->len; ++i) {
        const CtscNode* p = params->items[i];
        if (parameter_is_this_typing_only(p)) continue;
        if (!first) write_cstr(e, ", ");
        first = false;
        emit(e, p);
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitDecoratorsAndModifiers
 * (~4449) for class elements: transpiled JS retains only `static` and `async`;
 * accessibility / readonly / abstract modifiers are not printed (see
 * canEmitModifier / shouldPrintImportSpecifier in the TS compiler; ctsc
 * matches the common transpileModule output).
 */
static void emit_class_element_modifiers_for_js(Emitter* e, const CtscNodeArray* mods) {
    if (!mods) return;
    for (size_t i = 0; i < mods->len; ++i) {
        const CtscNode* m = mods->items[i];
        if (!m) continue;
        switch (m->kind) {
            case CTSC_SK_StaticKeyword:
                write_cstr(e, "static ");
                break;
            case CTSC_SK_AsyncKeyword:
                write_cstr(e, "async ");
                break;
            default:
                break;
        }
    }
}

static bool method_is_constructor(const CtscNode* m) {
    if (!m || m->kind != CTSC_SK_MethodDeclaration) return false;
    const CtscNode* name = m->data.methodDeclaration.name;
    return identifier_text_equals_ascii(name, "constructor");
}

static bool class_member_is_body_less_function_like(const CtscNode* m) {
    if (m->kind == CTSC_SK_MethodDeclaration
        && m->data.methodDeclaration.body == NULL) return true;
    if ((m->kind == CTSC_SK_GetAccessor || m->kind == CTSC_SK_SetAccessor)
        && m->data.accessorDeclaration.body == NULL) return true;
    return false;
}

static bool heritage_has_extends(const CtscClassDeclarationData* cd) {
    for (size_t i = 0; i < cd->heritage_clauses.len; ++i) {
        const CtscNode* hc = cd->heritage_clauses.items[i];
        if (hc && hc->kind == CTSC_SK_HeritageClause
            && hc->data.heritageClause.token == CTSC_SK_ExtendsKeyword) {
            return true;
        }
    }
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitHeritageClause (~4029)
 * and emitList(..., ClassHeritageClauses) before `{` in
 * emitClassDeclarationOrExpression (~3557).
 *     writeSpace(); writeTokenText(node.token); writeSpace();
 *     emitList(node, node.types, ListFormat.HeritageClauseTypes);
 * ClassHeritageClauses is SingleLine; each HeritageClause begins with a leading
 * space so `extends A implements B` prints without an extra list separator.
 */
static void emit_class_heritage_clauses(Emitter* e, const CtscClassDeclarationData* cd) {
    const CtscNodeArray* hcs = &cd->heritage_clauses;
    for (size_t hi = 0; hi < hcs->len; ++hi) {
        const CtscNode* hc = hcs->items[hi];
        if (!hc || hc->kind != CTSC_SK_HeritageClause) continue;
        const CtscHeritageClauseData* hcd = &hc->data.heritageClause;
        write_char(e, ' ');
        if (hcd->token == CTSC_SK_ExtendsKeyword) {
            write_cstr(e, "extends");
        } else if (hcd->token == CTSC_SK_ImplementsKeyword) {
            write_cstr(e, "implements");
        } else {
            continue;
        }
        write_char(e, ' ');
        const CtscNodeArray* types = &hcd->types;
        for (size_t ti = 0; ti < types->len; ++ti) {
            if (ti) write_cstr(e, ", ");
            emit(e, types->items[ti]);
        }
    }
}

static bool stmt_is_super_call(const CtscNode* s) {
    if (!s || s->kind != CTSC_SK_ExpressionStatement) return false;
    const CtscNode* ex = s->data.expressionStatement.expression;
    if (!ex || ex->kind != CTSC_SK_CallExpression) return false;
    const CtscNode* callee = ex->data.callExpression.expression;
    return callee && callee->kind == CTSC_SK_SuperKeyword;
}

static size_t super_call_prologue_end(const CtscNodeArray* stmts) {
    size_t i = 0;
    while (i < stmts->len && stmt_is_super_call(stmts->items[i])) i++;
    return i;
}

/*
 * Mirrors upstream transformers/ts.ts transformParameterWithPropertyAssignment
 * (~1430): `this.<name> = <name>;` for constructor parameter properties.
 */
static bool ctor_has_parameter_properties(const CtscNodeArray* params) {
    for (size_t i = 0; i < params->len; ++i) {
        const CtscNode* p = params->items[i];
        if (p && p->kind == CTSC_SK_Parameter && p->data.parameter.is_parameter_property) {
            return true;
        }
    }
    return false;
}

static void emit_this_param_property_assignment(Emitter* e, const CtscNode* param) {
    const CtscNode* name = param->data.parameter.name;
    if (!name || name->kind != CTSC_SK_Identifier) return;
    write_cstr(e, "this");
    write_char(e, '.');
    emit(e, name);
    write_cstr(e, " = ");
    emit(e, name);
    write_char(e, ';');
}

static void emit_this_property_initializer(Emitter* e, const CtscNode* prop) {
    const CtscNode* name = prop->data.propertyDeclaration.name;
    const CtscNode* init = prop->data.propertyDeclaration.initializer;
    write_cstr(e, "this");
    if (name->kind == CTSC_SK_Identifier) {
        write_char(e, '.');
        emit(e, name);
    } else if (name->kind == CTSC_SK_ComputedPropertyName) {
        write_char(e, '[');
        emit(e, name->data.computedPropertyName.expression);
        write_char(e, ']');
    } else if (name->kind == CTSC_SK_StringLiteral || name->kind == CTSC_SK_NumericLiteral) {
        write_char(e, '[');
        emit(e, name);
        write_char(e, ']');
    } else {
        write_char(e, '.');
        emit(e, name);
    }
    write_cstr(e, " = ");
    emit(e, init);
    write_char(e, ';');
}

/*
 * Mirrors upstream transformers/ts.ts class field lowering for static
 * PropertyDeclarations: emit `ClassName.prop = init` after the class body
 * (not `this.prop` in the constructor). Selfhost-derived 87_export_abstract.ts.
 */
static bool property_declaration_is_static(const CtscNode* prop) {
    if (!prop || prop->kind != CTSC_SK_PropertyDeclaration) return false;
    const CtscNodeArray* mods = &prop->data.propertyDeclaration.modifiers;
    for (size_t i = 0; i < mods->len; ++i) {
        const CtscNode* m = mods->items[i];
        if (m && m->kind == CTSC_SK_StaticKeyword) return true;
    }
    return false;
}

static void emit_static_property_initializer(Emitter* e, const CtscNode* class_name, const CtscNode* prop) {
    const CtscNode* name = prop->data.propertyDeclaration.name;
    const CtscNode* init = prop->data.propertyDeclaration.initializer;
    if (!class_name || !name || !init) return;
    emit(e, class_name);
    if (name->kind == CTSC_SK_Identifier) {
        write_char(e, '.');
        emit(e, name);
    } else if (name->kind == CTSC_SK_ComputedPropertyName) {
        write_char(e, '[');
        emit(e, name->data.computedPropertyName.expression);
        write_char(e, ']');
    } else if (name->kind == CTSC_SK_StringLiteral || name->kind == CTSC_SK_NumericLiteral) {
        write_char(e, '[');
        emit(e, name);
        write_char(e, ']');
    } else {
        write_char(e, '.');
        emit(e, name);
    }
    write_cstr(e, " = ");
    emit(e, init);
    write_char(e, ';');
}

static void emit_trailing_static_property_initializers(
    Emitter* e,
    const CtscClassDeclarationData* cd,
    const CtscNode* const* static_prop_buf,
    size_t n_stat) {
    if (!cd->name || n_stat == 0) return;
    for (size_t si = 0; si < n_stat; ++si) {
        write_raw_newline(e);
        emit_static_property_initializer(e, cd->name, static_prop_buf[si]);
    }
}

static void emit_merged_constructor_body(
    Emitter* e,
    const CtscNode** prop_inits,
    size_t nprops,
    const CtscNode* ctor_method,
    bool has_extends) {
    const CtscNodeArray* params = &ctor_method->data.methodDeclaration.parameters;
    const CtscNode* body = ctor_method->data.methodDeclaration.body;
    write_cstr(e, "constructor");
    write_char(e, '(');
    emit_list(e, params, ", ");
    write_char(e, ')');
    write_char(e, ' ');
    if (!body || body->kind != CTSC_SK_Block) return;
    write_char(e, '{');
    const CtscNodeArray* bstmts = &body->data.block.statements;
    size_t n = bstmts->len;
    size_t super_end = has_extends ? super_call_prologue_end(bstmts) : 0;
    bool has_pp = ctor_has_parameter_properties(params);

    if (n == 0 && nprops == 0 && !has_pp) {
        if (body->data.block.multi_line) {
            write_line(e);
        } else {
            write_char(e, ' ');
        }
        write_char(e, '}');
        return;
    }

    e->indent++;
    if (has_extends) {
        for (size_t j = 0; j < super_end; ++j) {
            write_line(e);
            emit(e, bstmts->items[j]);
        }
    }
    for (size_t i = 0; i < params->len; ++i) {
        const CtscNode* pm = params->items[i];
        if (!pm || pm->kind != CTSC_SK_Parameter || !pm->data.parameter.is_parameter_property) {
            continue;
        }
        write_line(e);
        emit_this_param_property_assignment(e, pm);
    }
    for (size_t pi = 0; pi < nprops; ++pi) {
        write_line(e);
        emit_this_property_initializer(e, prop_inits[pi]);
    }
    size_t start_rest = has_extends ? super_end : 0;
    for (size_t j = start_rest; j < n; ++j) {
        write_line(e);
        emit(e, bstmts->items[j]);
    }
    e->indent--;
    write_line(e);
    write_char(e, '}');
}

static void emit_synthetic_constructor_for_property_inits(
    Emitter* e, const CtscNode** prop_inits, size_t nprops) {
    write_cstr(e, "constructor() ");
    write_char(e, '{');
    e->indent++;
    for (size_t pi = 0; pi < nprops; ++pi) {
        write_line(e);
        emit_this_property_initializer(e, prop_inits[pi]);
    }
    e->indent--;
    write_line(e);
    write_char(e, '}');
}

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts iterateCommentRanges
 * (~826) + utilities.ts writeCommentRange (~7082) for the common cases. We
 * currently only invoke this at the boundary of an empty SourceFile, which
 * corresponds to tsc's emitBodyWithDetachedComments → emitLeadingComments
 * call at `detachedRange.end` (emitter.ts ~5944). For the SourceFile case
 * `detachedRange === node.statements`, and `statements.end === 0` whenever
 * the whole file is leading trivia (see parser.ts parseList).
 *
 * For a single-line `//` comment the body is copied verbatim and followed by
 * '\n' when a line break (possibly after spaces) follows. For a block
 * slash-star comment tsc emits either a trailing '\n' when a line break
 * follows or a trailing ' ' otherwise (emitter.ts emitLeadingComment ~6019).
 */
static void emit_source_file_leading_comments(Emitter* e, size_t pos) {
    if (!e->source) return;
    const uint16_t* t = e->source->data;
    size_t len = e->source->len;
    if (pos >= len) return;

    /* Mirrors upstream/TypeScript/src/compiler/scanner.ts iterateCommentRanges
     * (~826) with trailing=false: `collecting` starts as `pos === 0` (the
     * shebang special-case), and flips to true on every line break; comments
     * encountered while `!collecting` are scanned-over but NOT reported to
     * the caller. That is what makes the slash-star `foo` comment in
     * `\ SLASH-STAR foo SLASH ;` (parserSkippedTokens5) invisible to the
     * emitter: the first EmptyStatement's `pos` is 20 (the space after
     * `\`), so we enter with collecting=false, and there is no line break
     * between pos 20 and the block comment at pos 21 to flip it - the
     * comment is silently skipped, mirroring getLeadingCommentRanges(src,
     * 20) === undefined. */
    bool collecting = (pos == 0);
    while (pos < len) {
        uint16_t c = t[pos];
        if (c == 0x0D) {
            if (pos + 1 < len && t[pos + 1] == 0x0A) pos += 2; else pos++;
            collecting = true;
            continue;
        }
        if (c == 0x0A || c == 0x2028 || c == 0x2029) { pos++; collecting = true; continue; }
        if (c == 0x09 || c == 0x0B || c == 0x0C || c == 0x20) { pos++; continue; }
        /* BOM (U+FEFF) behaves as whitespace in tsc's iterateCommentRanges
         * (scanner.ts ~826 default branch: isWhiteSpaceLike is true for
         * byteOrderMark — see scanner.ts ~571). Skip it so that fixtures
         * whose first bytes are a UTF-8 BOM still pick up the subsequent
         * leading comment. */
        if (c == 0xFEFF) { pos++; continue; }
        if (c == '/' && pos + 1 < len && (t[pos + 1] == '/' || t[pos + 1] == '*')) {
            bool is_block = t[pos + 1] == '*';
            size_t start = pos;
            pos += 2;
            bool has_trailing_nl = false;
            if (!is_block) {
                while (pos < len && !is_line_break_u16(t[pos])) pos++;
                if (pos < len) has_trailing_nl = true;
            } else {
                while (pos + 1 < len && !(t[pos] == '*' && t[pos + 1] == '/')) pos++;
                if (pos + 1 < len) pos += 2;
                size_t scan = pos;
                while (scan < len) {
                    uint16_t d = t[scan];
                    if (is_line_break_u16(d)) { has_trailing_nl = true; break; }
                    if (d == 0x09 || d == 0x0B || d == 0x0C || d == 0x20) { scan++; continue; }
                    break;
                }
            }
            if (collecting) {
                write_u16(e, t + start, pos - start);
                if (has_trailing_nl) {
                    write_raw_newline(e);
                } else if (is_block) {
                    write_char(e, ' ');
                }
            }
            continue;
        }
        break;
    }
}

/*
 * Mirrors upstream utilities.ts nodeIsSynthesized (~5625): synthesized nodes
 * use negative or otherwise invalid TextRange positions.
 */
static bool ctsc_node_is_synthesized_for_emit(const CtscNode* n) {
    return !n || n->pos < 0 || n->end < 0;
}

/*
 * Mirrors upstream utilities.ts rangeStartPositionsAreOnSameLine (~7920) with
 * getStartPositionOfRange (includeComments=false) implemented as skipTrivia
 * on each node's pos (scanner.ts skipTrivia ~641).
 */
static bool range_start_positions_are_on_same_line(
    const CtscUtf16Buf* src, const CtscNode* range1, const CtscNode* range2) {
    if (!src) return true;
    size_t p1 = skip_trivia_u16(src, (size_t)range1->pos);
    size_t p2 = skip_trivia_u16(src, (size_t)range2->pos);
    return line_of_position(src, p1) == line_of_position(src, p2);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitCaseOrDefaultClauseRest
 * (~4007): one statement on the same source line as the clause uses
 * ColonToken + space + emitList without MultiLine|Indented; otherwise colon
 * then emitList with ListFormat.CaseOrDefaultClauseStatements.
 */
static void emit_case_clause_statements(Emitter* e, const CtscNode* parent_clause, const CtscNodeArray* stmts) {
    if (stmts->len == 0) {
        write_line(e);
        return;
    }
    bool emit_as_single = stmts->len == 1
        && (!e->source || ctsc_node_is_synthesized_for_emit(parent_clause)
            || ctsc_node_is_synthesized_for_emit(stmts->items[0])
            || range_start_positions_are_on_same_line(e->source, parent_clause, stmts->items[0]));
    if (emit_as_single) {
        write_char(e, ' ');
        emit(e, stmts->items[0]);
        return;
    }
    write_line(e);
    e->indent++;
    for (size_t i = 0; i < stmts->len; ++i) {
        write_line(e);
        emit(e, stmts->items[i]);
    }
    e->indent--;
}

/*
 * Mirrors upstream transformers/ts.ts getExpressionForPropertyName (~1216):
 * Identifier → StringLiteral(idText(name)); ComputedPropertyName → expression;
 * other property names clone as-is.
 */
static void emit_enum_property_name_expression(Emitter* e, const CtscNode* prop_name) {
    if (!prop_name) return;
    if (prop_name->kind == CTSC_SK_Identifier) {
        write_char(e, '"');
        write_string_literal_escaped(e, prop_name->data.identifier.text, prop_name->data.identifier.text_len,
                                    /*single_quote*/ false);
        write_char(e, '"');
        return;
    }
    if (prop_name->kind == CTSC_SK_ComputedPropertyName) {
        emit(e, prop_name->data.computedPropertyName.expression);
        return;
    }
    emit(e, prop_name);
}

static bool u16_parse_ascii_decimal_i64(const uint16_t* t, size_t len, int64_t* out) {
    int64_t v = 0;
    bool any = false;
    for (size_t i = 0; i < len; ++i) {
        uint16_t c = t[i];
        if (c == (uint16_t) '_') continue;
        if (c < (uint16_t) '0' || c > (uint16_t) '9') return false;
        any = true;
        v = v * 10 + (int64_t)(c - (uint16_t) '0');
    }
    if (!any) return false;
    *out = v;
    return true;
}

/*
 * Mirrors resolver.getEnumMemberValue + transformEnumMemberDeclarationValue
 * (transformers/ts.ts ~1913–1966) for the common literal-only shapes so the
 * next implicit member can use auto-increment (constantValue + 1).
 */
static bool enum_member_init_constant_integer(const CtscNode* init, int64_t* out) {
    if (!init) return false;
    if (init->kind == CTSC_SK_NumericLiteral) {
        const uint16_t* t;
        size_t len;
        if (init->data.numericLiteral.source_text && init->data.numericLiteral.source_text_len) {
            t = init->data.numericLiteral.source_text;
            len = init->data.numericLiteral.source_text_len;
        } else {
            t = init->data.numericLiteral.text;
            len = init->data.numericLiteral.text_len;
        }
        return u16_parse_ascii_decimal_i64(t, len, out);
    }
    if (init->kind == CTSC_SK_PrefixUnaryExpression
        && init->data.prefixUnaryExpression.operator_kind == CTSC_SK_MinusToken) {
        const CtscNode* op = init->data.prefixUnaryExpression.operand;
        int64_t v;
        if (op && enum_member_init_constant_integer(op, &v)) {
            *out = -v;
            return true;
        }
    }
    return false;
}

static void write_i64_decimal(Emitter* e, int64_t v) {
    if (v < 0) {
        write_char(e, '-');
        if (v == INT64_MIN) {
            write_cstr(e, "9223372036854775808");
            return;
        }
        v = -v;
    }
    if (v == 0) {
        write_char(e, '0');
        return;
    }
    char buf[32];
    int n = 0;
    uint64_t u = (uint64_t)v;
    while (u > 0) {
        buf[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    while (n > 0) write_char(e, buf[--n]);
}

static void emit_enum_member_value_and_bump_auto(Emitter* e, const CtscNode* init, int64_t* auto_num) {
    if (!init) {
        write_i64_decimal(e, *auto_num);
        (*auto_num)++;
        return;
    }
    emit(e, init);
    int64_t v;
    if (enum_member_init_constant_integer(init, &v)) {
        *auto_num = v + 1;
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts
 * shouldEmitBlockFunctionBodyOnSingleLine (~3476): function / arrow bodies
 * use emitBlockFunctionBody (~3516) instead of emitBlock (~3036).
 */
static bool block_function_body_should_emit_single_line(Emitter* e, const CtscNode* block) {
    if (!block || block->kind != CTSC_SK_Block) return false;
    if (block->data.block.multi_line) return false;
    const CtscNodeArray* ss = &block->data.block.statements;
    if (ss->len == 0) return true;
    if (!e->source) return ss->len == 1;
    if (lines_between_positions(e->source, (size_t)block->pos, (size_t)block->end) > 0) {
        return false;
    }
    for (size_t i = 1; i < ss->len; ++i) {
        const CtscNode* prev = ss->items[i - 1];
        const CtscNode* cur = ss->items[i];
        if (lines_between_positions(e->source, (size_t)prev->end, (size_t)cur->pos) > 0) {
            return false;
        }
    }
    if (ss->len > 0) {
        size_t open = skip_trivia_u16(e->source, (size_t)block->pos);
        const uint16_t* t = e->source->data;
        if (open < e->source->len && t[open] == (uint16_t)'{') {
            open++;
        }
        if (lines_between_positions(e->source, open, (size_t)ss->items[0]->pos) > 0) {
            return false;
        }
        const CtscNode* last = ss->items[ss->len - 1];
        if (lines_between_positions(e->source, (size_t)last->end, (size_t)block->end) > 0) {
            return false;
        }
    }
    return true;
}

/*
 * Mirrors upstream emitBlockFunctionBody (~3516) + emitBlockFunctionBodyWorker
 * (~3538): leading `{`, then either SingleLineFunctionBodyStatements
 * (~3543-3546) or MultiLineFunctionBodyStatements (~3549).
 *
 * `allow_single_line_compact` is true only for arrow-function bodies: tsc's
 * shouldEmitBlockFunctionBodyOnSingleLine (~3476) also consults
 * getLeadingLineTerminatorCount / getClosingLineTerminatorCount with
 * PreserveLines, which keeps ordinary FunctionDeclaration bodies multi-line
 * even when the source block is one line (see test_emitter throw/never case).
 * Arrow callbacks still compact to `{ stmt; }` when the source block fits
 * one line (selfhost-derived 101_inline_arrow_body.ts).
 */
static void emit_function_like_block(Emitter* e, const CtscNode* block, bool allow_single_line_compact) {
    if (!block || block->kind != CTSC_SK_Block) return;
    const CtscNodeArray* ss = &block->data.block.statements;
    bool single = allow_single_line_compact && block_function_body_should_emit_single_line(e, block);
    write_char(e, '{');
    e->indent++;
    if (ss->len == 0) {
        if (block->data.block.multi_line) {
            write_line(e);
        } else {
            write_char(e, ' ');
        }
        e->indent--;
        write_char(e, '}');
        return;
    }
    if (single) {
        /* emitBlockFunctionBodyOnSingleLine (~3534): emitBlockFunctionBody
         * already increased indent after `{`; the worker dips before the
         * horizontal list so embedded statements (emitEmbeddedStatement ~4568)
         * still indent one level past the outer statement. */
        e->indent--;
        write_char(e, ' ');
        for (size_t i = 0; i < ss->len; ++i) {
            if (i) write_char(e, ' ');
            emit(e, ss->items[i]);
        }
        write_char(e, ' ');
        e->indent++;
        e->indent--;
        write_char(e, '}');
        return;
    }
    for (size_t i = 0; i < ss->len; ++i) {
        write_line(e);
        emit(e, ss->items[i]);
    }
    e->indent--;
    write_line(e);
    write_char(e, '}');
}

static void emit(Emitter* e, const CtscNode* n) {
    if (!n) return;
    switch (n->kind) {
        case CTSC_SK_SourceFile: {
            const CtscNodeArray* ss = &n->data.sourceFile.statements;
            /* Mirrors emitter.ts emitSourceFile (~4299) →
             * emitBodyWithDetachedComments(node, statements, emitSourceFileWorker)
             * (~5922). The detached-comments pre-pass only emits the leading
             * comment ranges when a blank line separates them from the first
             * node (utilities.ts emitDetachedComments ~7063); for contiguous
             * leading comments like `// @target:es6\nlet`, tsc instead emits
             * them as part of the first statement's pipelineEmitWithComments
             * (~5795 → emitLeadingComments ~5976), which iterates the same
             * leading comment ranges starting at `pos = firstStatement.pos`
             * and writes each comment followed by a newline when it has a
             * trailing line break (emitLeadingComment ~6019). The effective
             * byte stream is identical in both cases: the comments and their
             * trailing newlines are written before the first statement. We
             * approximate that by replaying leading trivia at
             * `firstStatement.pos` (== 0 at the top of file) exactly once,
             * here, before any statement is emitted. For an empty file whose
             * entire content is leading trivia (statements.len == 0) we
             * replay at `statements_end` instead, mirroring the trailing
             * emitLeadingComments call at `detachedRange.end` (~5944). */
            /* emitBodyWithDetachedComments (emitter.ts ~5922): emitDetachedComments
             * (utilities.ts ~7012) on the statements list before emitSourceFileWorker.
             * Uses the first statement's pos (NodeArray.pos), not the first *emitted*
             * statement — otherwise a leading copyright block before elided `type`
             * aliases is never replayed (selfhost-derived 91_union_type_alias_object.ts).
             */
            size_t last_detached_comment_src_end = 0;
            bool detached_wrote = false;
            if (ss->len > 0) {
                detached_wrote = emit_detached_comments(
                    e, (size_t)ss->items[0]->pos, &last_detached_comment_src_end);
            }
            /* Skip leading statements that the TS-to-JS transformer
             * replaces with NotEmittedStatement (see
             * source_file_statement_is_dropped). emitLeadingComments at
             * pos=0 with isEmittedNode=false (emitter.ts ~5976) only
             * forwards triple-slash comments, so for the leaf case where
             * every leading statement is dropped we suppress comment
             * replay entirely. */
            size_t first_emitted = 0;
            while (first_emitted < ss->len
                   && source_file_statement_is_dropped(ss->items[first_emitted])) {
                first_emitted++;
            }
            /*
             * Leading trivia replay uses the first *emitted* statement when one
             * exists. When every statement is elided, tsc normally suppresses
             * non-`///` leading comments (overload-only file — from-upstream
             * 106_parserFunctionDeclaration3.ts). The exception is an ES
             * module whose only surface was type-only exports: the file stays a
             * module and still replays header comments before the synthetic
             * `export {};` (selfhost-derived 97_empty_module_marker.ts).
             */
            if (ss->len > 0) {
                bool replay_leading;
                size_t idx;
                if (first_emitted < ss->len) {
                    replay_leading = true;
                    idx = first_emitted;
                } else if (source_file_is_external_module(n)) {
                    replay_leading = true;
                    idx = 0;
                } else {
                    replay_leading = false;
                    idx = 0;
                }
                if (replay_leading) {
                    size_t leading_pos = (size_t)ss->items[idx]->pos;
                    /*
                     * When the first emitted node spans pos=0 (leading trivia of its
                     * first token starts at the beginning of the file), the detached
                     * pass and emit_source_file_leading_comments would both replay
                     * the same header (utilities.ts emitDetachedComments vs
                     * emitLeadingComments). Skip the second pass if detached already
                     * wrote (selfhost-derived 92_import_named.ts). When leading_pos > 0
                     * (e.g. elided type aliases before `export` in 91_union_type_alias_object.ts)
                     * the leading replay is still needed for the non-detached case.
                     */
                    if (!detached_wrote || leading_pos != 0) {
                        emit_source_file_leading_comments(e, leading_pos);
                    } else if (e->source) {
                        /*
                         * Detached comments at pos 0 suppress emit_source_file_leading_comments
                         * to avoid duplicate headers (selfhost-derived 92_import_named.ts).
                         * Mirror forEachLeadingCommentWithoutDetachedComments (emitter.ts ~6128):
                         * resume leading-comment iteration from last(detachedComments).end.
                         * iterateCommentRanges consumes intervening line breaks without writing
                         * them (scanner.ts ~850), so a blank line in the source between the
                         * detached header and the first statement is not replayed (tsc
                         * transpileModule output for selfhost-derived 93_import_default.ts).
                         */
                        emit_source_file_leading_comments(e, last_detached_comment_src_end);
                    }
                }
            } else if (ss->len == 0) {
                emit_source_file_leading_comments(
                    e, (size_t)n->data.sourceFile.statements_end);
            }
            for (size_t i = 0; i < ss->len; ++i) {
                if (source_file_statement_is_dropped(ss->items[i])) continue;
                emit(e, ss->items[i]);
                write_raw_newline(e);
            }
            if (source_file_needs_synthetic_empty_export(n, ss)) {
                write_cstr(e, "export {};");
                write_raw_newline(e);
            }
            return;
        }
        case CTSC_SK_EmptyStatement:
            write_char(e, ';');
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitSemicolonClassElement (~2322):
         *     writeTrailingSemicolon();
         * A bare `;` inside a class body (parser.ts parseClassElement ~8071:
         * `if (token() === SyntaxKind.SemicolonToken) { nextToken();
         * return finishNode(factory.createSemicolonClassElement(), pos); }`)
         * round-trips through the printer as a lone semicolon; the
         * surrounding indent + newlines come from the ClassMembers list
         * format (Indented | MultiLine, types.ts ~10177), which the
         * ClassDeclaration/ClassExpression emitter above already applies
         * around each member. For the 107_classWithSemicolonClassElement
         * fixture (`class C {\n    ;\n}`) this produces the expected
         * indented `;` between the braces.
         */
        case CTSC_SK_SemicolonClassElement:
            write_char(e, ';');
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitIfStatement
         * (~3073):
         *     writeKeyword("if"); writeSpace();
         *     writePunctuation("(");
         *     emitExpression(node.expression);
         *     writePunctuation(")");
         *     emitEmbeddedStatement(node, node.thenStatement);
         *     if (node.elseStatement) {
         *         writeLineOrSpace(...);        // space under transpileModule
         *         writeKeyword("else");
         *         if (node.elseStatement.kind === SyntaxKind.IfStatement) {
         *             writeSpace(); emit(node.elseStatement);
         *         } else {
         *             emitEmbeddedStatement(node, node.elseStatement);
         *         }
         *     }
         *
         * emitEmbeddedStatement (~4568) writes a single leading space before
         * emit(node) when the child is a Block (isBlock) or when the parent
         * has the SingleLine emit flag; otherwise it drops to a writeLine +
         * increaseIndent + emit + decreaseIndent shape. preserveSourceNewlines
         * is undefined under ts.transpileModule (our oracle), so the simple
         * "Block → space, anything else → newline+indent" split fully covers
         * the fixtures we currently exercise.
         *
         * writeLineOrSpace (emitter.ts ~4579) degenerates to writeSpace when
         * preserveSourceNewlines is false / undefined, so an `else` branch is
         * preceded by a single space, matching tsc's `} else {` layout.
         */
        case CTSC_SK_IfStatement: {
            const CtscNode* cond = n->data.ifStatement.expression;
            const CtscNode* thenS = n->data.ifStatement.thenStatement;
            const CtscNode* elseS = n->data.ifStatement.elseStatement;
            write_cstr(e, "if (");
            emit(e, cond);
            write_char(e, ')');
            if (thenS && thenS->kind == CTSC_SK_Block) {
                write_char(e, ' ');
                emit(e, thenS);
            } else {
                write_line(e);
                e->indent++;
                emit(e, thenS);
                e->indent--;
            }
            if (elseS) {
                /* writeLineOrSpace → writeSpace under transpileModule. */
                write_char(e, ' ');
                write_cstr(e, "else");
                if (elseS->kind == CTSC_SK_IfStatement) {
                    write_char(e, ' ');
                    emit(e, elseS);
                } else if (elseS->kind == CTSC_SK_Block) {
                    write_char(e, ' ');
                    emit(e, elseS);
                } else {
                    write_line(e);
                    e->indent++;
                    emit(e, elseS);
                    e->indent--;
                }
            }
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitForStatement
         * (~3120):
         *     writeKeyword("for"); writeSpace();
         *     writePunctuation("(");
         *     emitForBinding(node.initializer);              // emit(list) or emitExpression
         *     writePunctuation(";");
         *     emitExpressionWithLeadingSpace(node.condition); // space + expr, nothing when absent
         *     writePunctuation(";");
         *     emitExpressionWithLeadingSpace(node.incrementor);
         *     writePunctuation(")");
         *     emitEmbeddedStatement(node, node.statement);
         *
         * emitExpressionWithLeadingSpace (~4554) is a no-op when the sub-node
         * is undefined, which is why `for (var of; ;) { }` becomes the
         * space-free `for (var of;;) { }` in tsc's output — the empty
         * condition skips the space-before-cond, the empty incrementor skips
         * the space-before-inc, and the between-semicolons space from the
         * source is not preserved. emitEmbeddedStatement (~4568) writes a
         * single leading space before a Block body.
         */
        case CTSC_SK_ForStatement: {
            const CtscNode* init  = n->data.forStatement.initializer;
            const CtscNode* cond  = n->data.forStatement.condition;
            const CtscNode* inc   = n->data.forStatement.incrementor;
            const CtscNode* body  = n->data.forStatement.statement;
            write_cstr(e, "for (");
            if (init) emit(e, init);
            write_char(e, ';');
            if (cond) { write_char(e, ' '); emit(e, cond); }
            write_char(e, ';');
            if (inc) { write_char(e, ' '); emit(e, inc); }
            write_char(e, ')');
            if (body && body->kind == CTSC_SK_Block) {
                write_char(e, ' ');
                emit(e, body);
            } else if (body && body->kind == CTSC_SK_EmptyStatement) {
                write_line(e);
                e->indent++;
                emit(e, body);
                e->indent--;
            } else {
                write_line(e);
                e->indent++;
                emit(e, body);
                e->indent--;
            }
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitForInStatement
         * (~3133) and emitForOfStatement (~3146):
         *     writeKeyword("for"); writeSpace();
         *     [emitWithTrailingSpace(node.awaitModifier); ]  // for-of only
         *     writePunctuation("(");
         *     emitForBinding(node.initializer);              // emit list or expression
         *     writeSpace();
         *     writeKeyword("in" | "of");
         *     writeSpace();
         *     emitExpression(node.expression);
         *     writePunctuation(")");
         *     emitEmbeddedStatement(node, node.statement);
         *
         * emitForBinding (~3160): when the initializer is a VariableDeclarationList
         * (even with zero declarations, e.g. the `for (var of X)` recovery path —
         * parser.ts parseVariableDeclarationList ~7705 createMissingList) the
         * printer still emits `var ` via emitVariableDeclarationList (~3409):
         * head keyword + writeSpace + emitList(declarations). For empty
         * declarations emitList writes nothing, producing exactly `var ` (one
         * trailing space) — followed by the emitForOfStatement writeSpace before
         * `of`, which is why the expected byte stream has TWO spaces between
         * `var` and `of`.
         *
         * emitEmbeddedStatement (~4568) matches the ForStatement shape we already
         * use above: single leading space before a Block, otherwise newline +
         * indent. ctsc has not modelled awaitModifier yet (no active fixture
         * uses `for await (... of ...)`); extend this case when one unlocks.
         */
        case CTSC_SK_ForInStatement:
        case CTSC_SK_ForOfStatement: {
            const CtscNode* init = n->data.forInOrOfStatement.initializer;
            const CtscNode* expr = n->data.forInOrOfStatement.expression;
            const CtscNode* body = n->data.forInOrOfStatement.statement;
            const char* kw = (n->kind == CTSC_SK_ForInStatement) ? "in" : "of";
            write_cstr(e, "for (");
            if (init) emit(e, init);
            write_char(e, ' ');
            write_cstr(e, kw);
            write_char(e, ' ');
            if (expr) emit(e, expr);
            write_char(e, ')');
            if (body && body->kind == CTSC_SK_Block) {
                write_char(e, ' ');
                emit(e, body);
            } else {
                write_line(e);
                e->indent++;
                emit(e, body);
                e->indent--;
            }
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitDoStatement
         * (~3101) + emitWhileClause (~3093):
         *     writeKeyword("do"); emitEmbeddedStatement(node, node.statement);
         *     if (isBlock(node.statement) && !preserveSourceNewlines) writeSpace();
         *     else writeLineOrSpace(node, node.statement, node.expression);
         *     emitWhileClause(node, node.statement.end);
         *     writeTrailingSemicolon();
         *
         * emitEmbeddedStatement (~4568): space + Block; else newline + indent +
         * emit (+ EmptyStatement pipeline). Under transpileModule,
         * writeLineOrSpace (~4957) is writeLine when not SingleLine / no
         * preserveSourceNewlines.
         */
        case CTSC_SK_DoStatement: {
            const CtscNode* stmt = n->data.doStatement.statement;
            const CtscNode* expr = n->data.doStatement.expression;
            write_cstr(e, "do");
            if (stmt && stmt->kind == CTSC_SK_Block) {
                write_char(e, ' ');
                emit(e, stmt);
                write_char(e, ' ');
            } else if (stmt && stmt->kind == CTSC_SK_EmptyStatement) {
                write_line(e);
                e->indent++;
                emit(e, stmt);
                e->indent--;
                write_line(e);
            } else {
                write_line(e);
                e->indent++;
                emit(e, stmt);
                e->indent--;
                write_line(e);
            }
            write_cstr(e, "while (");
            if (expr) emit(e, expr);
            write_cstr(e, ");");
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitWhileClause
         * (~3093) + emitWhileStatement (~3115):
         *     writeKeyword("while"); writeSpace();
         *     writePunctuation("(");
         *     emitExpression(node.expression);
         *     writePunctuation(")");
         *     emitEmbeddedStatement(node, node.statement);
         *
         * emitEmbeddedStatement matches the ForStatement branch above: space
         * before a Block body, newline + indent otherwise.
         */
        case CTSC_SK_WhileStatement: {
            const CtscNode* cond = n->data.whileStatement.expression;
            const CtscNode* body = n->data.whileStatement.statement;
            write_cstr(e, "while (");
            if (cond) emit(e, cond);
            write_char(e, ')');
            if (body && body->kind == CTSC_SK_Block) {
                write_char(e, ' ');
                emit(e, body);
            } else if (body && body->kind == CTSC_SK_EmptyStatement) {
                write_line(e);
                e->indent++;
                emit(e, body);
                e->indent--;
            } else {
                write_line(e);
                e->indent++;
                emit(e, body);
                e->indent--;
            }
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitDebuggerStatement
         * (~3393): writeToken(DebuggerKeyword) + writeTrailingSemicolon.
         */
        case CTSC_SK_DebuggerStatement:
            write_cstr(e, "debugger;");
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitTryStatement
         * (~3377):
         *     emitTokenWithComment(TryKeyword, ...); writeSpace();
         *     emit(node.tryBlock);
         *     if (node.catchClause) {
         *         writeLineOrSpace(node, node.tryBlock, node.catchClause);
         *         emit(node.catchClause);
         *     }
         *     if (node.finallyBlock) {
         *         writeLineOrSpace(node, node.catchClause || node.tryBlock, node.finallyBlock);
         *         emitTokenWithComment(FinallyKeyword, ...); writeSpace();
         *         emit(node.finallyBlock);
         *     }
         *
         * writeLineOrSpace (~4957): under ts.transpileModule (our oracle)
         * `preserveSourceNewlines` is undefined and TryStatement does not
         * carry EmitFlags.SingleLine, so the call unconditionally emits
         * `writeLine()` — i.e. a single newline between tryBlock/catchClause
         * and the following keyword (`catch`/`finally`).
         *
         * emitCatchClause (~4036) is implemented as CTSC_SK_CatchClause below.
         */
        case CTSC_SK_TryStatement: {
            const CtscNode* tryBlock     = n->data.tryStatement.tryBlock;
            const CtscNode* catchClause  = n->data.tryStatement.catchClause;
            const CtscNode* finallyBlock = n->data.tryStatement.finallyBlock;
            write_cstr(e, "try ");
            if (tryBlock) emit(e, tryBlock);
            if (catchClause) {
                write_line(e);
                emit(e, catchClause);
            }
            if (finallyBlock) {
                write_line(e);
                write_cstr(e, "finally ");
                emit(e, finallyBlock);
            }
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitCatchClause
         * (~4036): CatchKeyword + optional ( variableDeclaration ) + block.
         * Type annotations on the binding are elided via VariableDeclaration emit
         * (name only; no type node in JS output).
         */
        case CTSC_SK_CatchClause: {
            write_cstr(e, "catch");
            write_char(e, ' ');
            if (n->data.catchClause.variableDeclaration) {
                write_char(e, '(');
                emit(e, n->data.catchClause.variableDeclaration);
                write_char(e, ')');
                write_char(e, ' ');
            }
            emit(e, n->data.catchClause.block);
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitSwitchStatement
         * (~3354) and emitCaseBlock (~3657) / emitCaseClause (~3994) /
         * emitDefaultClause (~4002).
         */
        case CTSC_SK_SwitchStatement: {
            write_cstr(e, "switch (");
            emit(e, n->data.switchStatement.expression);
            write_cstr(e, ") ");
            emit(e, n->data.switchStatement.caseBlock);
            return;
        }
        case CTSC_SK_CaseBlock: {
            write_char(e, '{');
            const CtscNodeArray* clauses = &n->data.caseBlock.clauses;
            if (clauses->len == 0) {
                if (n->data.caseBlock.multi_line) {
                    write_line(e);
                } else {
                    write_char(e, ' ');
                }
                write_char(e, '}');
                return;
            }
            e->indent++;
            for (size_t i = 0; i < clauses->len; ++i) {
                write_line(e);
                emit(e, clauses->items[i]);
            }
            e->indent--;
            write_line(e);
            write_char(e, '}');
            return;
        }
        case CTSC_SK_CaseClause: {
            write_cstr(e, "case ");
            emit(e, n->data.caseClause.expression);
            write_char(e, ':');
            emit_case_clause_statements(e, n, &n->data.caseClause.statements);
            return;
        }
        case CTSC_SK_DefaultClause: {
            write_cstr(e, "default");
            write_char(e, ':');
            emit_case_clause_statements(e, n, &n->data.defaultClause.statements);
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitLabeledStatement (~3364):
         *     emit(node.label);
         *     emitTokenWithComment(SyntaxKind.ColonToken, node.label.end, writePunctuation, node);
         *     writeSpace();
         *     emit(node.statement);
         */
        case CTSC_SK_LabeledStatement: {
            const CtscNode* lab = n->data.labeledStatement.label;
            const CtscNode* stmt = n->data.labeledStatement.statement;
            if (lab) emit(e, lab);
            write_char(e, ':');
            write_char(e, ' ');
            if (stmt) emit(e, stmt);
            return;
        }
        case CTSC_SK_ExpressionStatement:
            /* Mirrors upstream/TypeScript/src/compiler/emitter.ts
             * emitExpressionStatement (~3064): emit the expression then
             * writeTrailingSemicolon. The pipelineEmitWithComments
             * wrapper (~5795) then emits any trailing comments at
             * node.end via emitCommentsAfterNode (~5816) then
             * emitTrailingComments (~6047). For
             * 106_parserGreaterThanTokenAmbiguity13.ts (source
             * `1 >>SLASH-STAR SLASH= 2;`) the first ExpressionStatement
             * ends at the missing identifier zero-width end (pos 23),
             * and the block comment that follows at pos 23 is emitted
             * as a trailing comment, yielding `1 >> ; SLASH-STAR SLASH`
             * before the statement-separator newline. The trailing scan
             * stops at the `=` that follows the comment (not whitespace,
             * not another comment), so the comment hasTrailingNewLine is
             * false and the separator newline comes from the SourceFile
             * loop write_raw_newline. */
            emit(e, n->data.expressionStatement.expression);
            write_char(e, ';');
            emit_trailing_comments_after(e, (size_t)n->end);
            return;
        case CTSC_SK_Identifier:
            write_u16(e, n->data.identifier.text, n->data.identifier.text_len);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * pipelineEmitWithHintWorker (~2027): for a primary-expression leaf
         * whose kind is a keyword (ThisKeyword / SuperKeyword / NullKeyword /
         * TrueKeyword / FalseKeyword) the default branch falls through to
         * `writeTokenNode(node, writeKeyword)` (~4939), which writes the
         * canonical token text via `tokenToString(kind)` (scanner.ts ~126).
         * The parser builds these as token-only leaves (parser.c
         * parse_primary ~85), so the emitter just writes the lowercase
         * keyword spelling.
         */
        case CTSC_SK_ThisKeyword:
            write_cstr(e, "this");
            return;
        case CTSC_SK_SuperKeyword:
            write_cstr(e, "super");
            return;
        case CTSC_SK_NullKeyword:
            write_cstr(e, "null");
            return;
        case CTSC_SK_TrueKeyword:
            write_cstr(e, "true");
            return;
        case CTSC_SK_FalseKeyword:
            write_cstr(e, "false");
            return;
        case CTSC_SK_NumericLiteral:
            /* Mirrors upstream/TypeScript/src/compiler/utilities.ts getLiteralText
             * (~1980) → canUseOriginalText (~2036): when the numeric literal has
             * a parent, is not synthesized, and does not carry TokenFlags.IsInvalid
             * (Octal / ContainsLeadingZero), the emitter writes the on-disk
             * lexeme verbatim; otherwise it falls back to node.text, i.e. the
             * canonical tokenValue recorded by scanNumber (`"" + +result`).
             *
             * Parser sets source_text to NULL when the scanner suppressed
             * `current.text` (invalid literals), so a null check here is the
             * equivalent of testing `!(numericLiteralFlags & IsInvalid)`. */
            if (n->data.numericLiteral.source_text && n->data.numericLiteral.source_text_len) {
                write_u16(e, n->data.numericLiteral.source_text, n->data.numericLiteral.source_text_len);
            } else {
                write_u16(e, n->data.numericLiteral.text, n->data.numericLiteral.text_len);
            }
            return;
        case CTSC_SK_NoSubstitutionTemplateLiteral:
        case CTSC_SK_TemplateHead:
        case CTSC_SK_TemplateMiddle:
        case CTSC_SK_TemplateTail:
            /* Mirrors upstream/TypeScript/src/compiler/emitter.ts emitLiteral
             * (~2118) → getLiteralText (utilities.ts ~1980): for a terminated
             * template-literal-like token with a parent, canUseOriginalText
             * (~2036) returns true, so the emitter writes the on-disk lexeme
             * verbatim (including the surrounding backticks / `${` / `}`
             * delimiters the scanner captured in `text`). For a missing
             * TemplateTail synthesised by parseLiteralOfTemplateSpan (~3713)
             * the parser leaves `text` NULL, which write_u16 handles as a
             * zero-byte write — matching getSourceTextOfNodeFromSourceFile
             * on a zero-width node.
             *
             * Under transpileModule (printerOptions.terminateUnterminatedLiterals
             * is unset) the `IsInvalid` / `isUnterminated` branches do not
             * apply, so we do not need to re-escape here. */
            write_u16(e, n->data.templateLiteralLike.text, n->data.templateLiteralLike.text_len);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitTemplateExpression (~2966):
         *     emit(node.head);
         *     emitList(node, node.templateSpans, ListFormat.TemplateExpressionSpans);
         * where TemplateExpressionSpans has no separator / delimiter bits —
         * spans are concatenated back-to-back. Each span emits its
         * expression (which appears inside `${...}`) followed by its
         * literal (TemplateMiddle or TemplateTail), whose raw lexeme already
         * begins with `}` and ends with either `${` or `` ` ``.
         */
        case CTSC_SK_TemplateExpression: {
            emit(e, n->data.templateExpression.head);
            const CtscNodeArray* spans = &n->data.templateExpression.templateSpans;
            for (size_t i = 0; i < spans->len; ++i) emit(e, spans->items[i]);
            return;
        }
        /* Mirrors emitter.ts emitTemplateSpan (~3027):
         *     emitExpression(node.expression);
         *     emit(node.literal);
         */
        case CTSC_SK_TemplateSpan:
            emit(e, n->data.templateSpan.expression);
            emit(e, n->data.templateSpan.literal);
            return;
        /* Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitTaggedTemplateExpression (~2722): emit tag, space, template. */
        case CTSC_SK_TaggedTemplateExpression:
            emit(e, n->data.taggedTemplateExpression.tag);
            write_char(e, ' ');
            emit(e, n->data.taggedTemplateExpression.template_);
            return;
        case CTSC_SK_RegularExpressionLiteral:
            /* Mirrors upstream/TypeScript/src/compiler/emitter.ts emitLiteral
             * (~2118), which is dispatched from pipelineEmitWithHintWorker
             * (~1929-1932): RegularExpressionLiteral falls into the same
             * literal-text path as StringLiteral / NoSubstitutionTemplateLiteral
             * via getLiteralTextOfNode → getSourceTextOfNodeFromSourceFile.
             * For unterminated regex tokens (parserMissingToken2: `/ b;`) tsc's
             * scanner records the trimmed literal text (see scanner.ts
             * ~2528 reScanSlashToken recovery), and the emitter writes it
             * back verbatim. Matches our scanner.c re_scan_slash_token output. */
            write_u16(e, n->data.regularExpressionLiteral.text, n->data.regularExpressionLiteral.text_len);
            return;
        case CTSC_SK_StringLiteral: {
            /* tsc: getLiteralText → escapeString (utilities.ts ~6207). */
            char q = n->data.stringLiteral.single_quote ? '\'' : '"';
            write_char(e, q);
            write_string_literal_escaped(e, n->data.stringLiteral.value, n->data.stringLiteral.value_len,
                                         n->data.stringLiteral.single_quote);
            write_char(e, q);
            return;
        }
        case CTSC_SK_VariableStatement:
            if (n->data.variableStatement.has_export) write_cstr(e, "export ");
            emit(e, n->data.variableStatement.declarationList);
            write_char(e, ';');
            return;
        case CTSC_SK_VariableDeclarationList: {
            int f = n->data.variableDeclarationList.flags;
            if      (f & 2) write_cstr(e, "const ");
            else if (f & 1) write_cstr(e, "let ");
            else            write_cstr(e, "var ");
            emit_list(e, &n->data.variableDeclarationList.declarations, ", ");
            return;
        }
        case CTSC_SK_VariableDeclaration:
            emit(e, n->data.variableDeclaration.name);
            if (n->data.variableDeclaration.initializer) {
                write_cstr(e, " = ");
                emit(e, n->data.variableDeclaration.initializer);
            }
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * createEmitBinaryExpression (~2839) / onOperator (~2884):
         *     writeLinesAndIndent(linesBeforeOperator, isCommaOperator);
         *     writeTokenNode(operatorToken);
         *     writeLinesAndIndent(linesAfterOperator,
         *                         writeSpaceIfNotIndenting = true);
         * with `linesBeforeOperator = getLinesBetweenNodes(node, node.left,
         * operatorToken)` and `linesAfterOperator = getLinesBetweenNodes(node,
         * operatorToken, node.right)` (~5196). On exit the trampoline runs
         * decreaseIndentIf (~5001) on both counts. Under the transpileModule
         * default (preserveSourceNewlines === undefined) each of those calls
         * reduces to `rangeEndIsOnSameLineAsRangeStart(a, b) ? 0 : 1` where
         * `rangeStart(b) = skipTrivia(b.pos)` (utilities.ts ~7939 + ~7965).
         *
         * ctsc does not yet model an explicit operatorToken node, but the
         * invariant we rely on holds for every BinaryExpression emitted by
         * the parser:
         *     operatorToken.pos === left.end
         *     operatorToken.end === right.pos
         * (see upstream parser.ts parseBinaryExpressionRest ~5080 + the
         * parseTokenNode call at ~5117, which sets full-start from the
         * scanner's current token position). Substituting those, the two
         * line counts collapse to a single `skipTrivia` probe at the left /
         * right sibling boundary — which is what 105_parserGreaterThan-
         * TokenAmbiguity4 (`1 >\n> 2;`) exercises:
         *   - inner binary: missing-identifier right at 23,23 →
         *       linesAfter = 1 (skipTrivia(23) → 25 across `\r\n`),
         *   - outer binary: left.end = 23 →
         *       linesBefore = 1 (skipTrivia(23) → 25 across `\r\n`).
         * Both produce a `writeLine` + indented right-hand side, and the
         * writer's collapsing `writeLine` (utilities.ts ~6366) merges the
         * two newlines into one, yielding `1 >\n    > 2;`.
         */
        case CTSC_SK_BinaryExpression: {
            const CtscNode* left  = n->data.binaryExpression.left;
            const CtscNode* right = n->data.binaryExpression.right;
            int left_end  = left  ? left->end  : n->pos;
            int right_pos = right ? right->pos : n->end;
            int lines_before = 0;
            int lines_after  = 0;
            if (e->source) {
                /* linesBeforeOperator: left.end → skipTrivia(op.pos == left.end). */
                size_t bstart = skip_trivia_u16(e->source, (size_t)left_end);
                lines_before = lines_between_positions(e->source, (size_t)left_end, bstart) > 0 ? 1 : 0;
                /* linesAfterOperator: op.end (== right.pos) → skipTrivia(right.pos). */
                size_t astart = skip_trivia_u16(e->source, (size_t)right_pos);
                lines_after  = lines_between_positions(e->source, (size_t)right_pos, astart) > 0 ? 1 : 0;
            }
            emit(e, left);
            /*
             * Mirrors upstream/TypeScript/src/compiler/emitter.ts
             * pipelineEmitWithComments (~5795) -> emitCommentsAfterNode
             * (~5816) -> emitTrailingCommentsOfNode (~5868) ->
             * emitTrailingComments (~6047): on exit of each emitted node the
             * printer replays trailing comments at node.end, filtered by the
             * ambient containerEnd (forEachTrailingCommentToEmit ~6117: emit
             * only when end != containerEnd). For a BinaryExpression the
             * trampoline's emitCommentsBeforeNode (~5805) sets containerEnd
             * to the node's own end, which SUPPRESSES the per-operator
             * trailing emission onOperator would otherwise try at
             * operatorToken.end (createEmitBinaryExpression ~2891). Only when
             * we pop back to the parent frame -- i.e. here, after emitting
             * `left` but before writing the current node's operator -- is the
             * inner node's trailing comment actually flushed, because the
             * parent's containerEnd differs from left.end.
             *
             * 106_parserGreaterThanTokenAmbiguity3 exercises this exactly:
             * the source `1 >SLASH-STAR SLASH> 2;` parses as
             * (1 > <missing>) > 2 with inner.end == <missing>.end == 22 (the
             * start of the block comment). Trailing at 22 emits a space
             * prefix plus the comment body with no newline
             * (hasTrailingNewLine = false, since the scan stops at `>` before
             * any line break). Same shape for 106_parserGreaterThanTokenAmbiguity8.
             *
             * Calling this unconditionally here is byte-compatible with
             * upstream for non-nested `left` as well: the trailing scan from
             * left.end stops at the first non-trivia / non-comment character,
             * so a plain whitespace gap before the operator is a no-op.
             *
             * 106_parserGreaterThanTokenAmbiguity13 (`1 >>SLASH-STAR SLASH= 2;`)
             * is unaffected here: there the block comment follows the outer
             * ExpressionStatement end (the `= 2;` fragment splits off into a
             * second statement via error recovery) and is emitted by
             * emit_trailing_comments_after on the ExpressionStatement itself.
             */
            if (left) emit_trailing_comments_after(e, (size_t)left->end);
            /* Mirrors upstream/TypeScript/src/compiler/emitter.ts onOperator
             * (~2884):
             *     const isCommaOperator = operatorToken.kind !== CommaToken;
             *     writeLinesAndIndent(linesBeforeOperator, isCommaOperator);
             * That is, the `writeSpaceIfNotIndenting` flag on the LEFT side of
             * the operator is `true` for every BinaryExpression EXCEPT those
             * whose operator is `,` — a comma operator renders tight against
             * its left operand (no leading space), matching JS convention
             * `a, b` not `a , b`. The flag on the RIGHT side is unconditional
             * `true`, which yields the single separating space in `, b`. */
            bool space_before_operator =
                n->data.binaryExpression.operator_kind != CTSC_SK_CommaToken;
            write_lines_and_indent(e, lines_before, space_before_operator);
            write_cstr(e, op_text(n->data.binaryExpression.operator_kind));
            write_lines_and_indent(e, lines_after, /*writeSpaceIfNotIndenting*/ true);
            emit(e, right);
            decrease_indent_if(e, lines_before, lines_after);
            return;
        }
        case CTSC_SK_CallExpression:
            emit(e, n->data.callExpression.expression);
            write_char(e, '(');
            emit_list(e, &n->data.callExpression.arguments, ", ");
            write_char(e, ')');
            return;
        /*
         * ts.transpileModule strips assertion types before print; emitted JS
         * keeps only the value (mirrors transformers' erase of AsExpression).
         */
        case CTSC_SK_AsExpression:
            emit(e, n->data.asExpression.expression);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitConditionalExpression (~2946):
         *     const linesBeforeQuestion = getLinesBetweenNodes(node, condition, questionToken);
         *     const linesAfterQuestion  = getLinesBetweenNodes(node, questionToken, whenTrue);
         *     const linesBeforeColon    = getLinesBetweenNodes(node, whenTrue, colonToken);
         *     const linesAfterColon     = getLinesBetweenNodes(node, colonToken, whenFalse);
         *     emitExpression(condition, parenthesizeConditionOfConditionalExpression);
         *     writeLinesAndIndent(linesBeforeQuestion, true); emit(questionToken);
         *     writeLinesAndIndent(linesAfterQuestion, true);  emitExpression(whenTrue, ...);
         *     decreaseIndentIf(linesBeforeQuestion, linesAfterQuestion);
         *     writeLinesAndIndent(linesBeforeColon, true);    emit(colonToken);
         *     writeLinesAndIndent(linesAfterColon, true);     emitExpression(whenFalse, ...);
         *     decreaseIndentIf(linesBeforeColon, linesAfterColon);
         *
         * Under `ts.transpileModule` the oracle uses preserveSourceNewlines=undefined,
         * so getLinesBetweenNodes (~5196) reduces to
         * `rangeEndIsOnSameLineAsRangeStart(a, b) ? 0 : 1`, with range2.start
         * taken through skipTrivia (utilities.ts ~7965).
         *
         * ctsc does not materialise the Question/Colon token nodes; we use the
         * well-formed position identities
         *     questionToken.pos == condition.end,
         *     questionToken.end == whenTrue.pos,
         *     colonToken.pos    == whenTrue.end,
         *     colonToken.end    == whenFalse.pos
         * which hold for every ConditionalExpression built by parser.ts
         * parseConditionalExpressionRest (~5058): parseTokenNode captures
         * full_start = scanner.getTokenFullStart() before consuming the token,
         * and finishNode sets end to scanner.getTokenFullStart() AFTER. The
         * four line counts therefore collapse to two distinct skipTrivia
         * probes: at condition.end (== questionToken.pos) for linesBefore-
         * Question AND linesAfterQuestion, and at whenTrue.end (== colonToken
         * .pos) for linesBeforeColon AND linesAfterColon.
         *
         * parenthesizeConditionOfConditionalExpression / parenthesizeBranch-
         * OfConditionalExpression only kick in for operands that would
         * re-associate (assignment/comma at outer-right, or `?`-nesting); the
         * operand shapes the parser currently produces (identifiers, numeric
         * literals, binary comparisons, new-expressions) render unchanged, so
         * we delegate directly to emit().
         */
        case CTSC_SK_ConditionalExpression: {
            const CtscNode* cond      = n->data.conditionalExpression.condition;
            const CtscNode* whenTrue  = n->data.conditionalExpression.whenTrue;
            const CtscNode* whenFalse = n->data.conditionalExpression.whenFalse;
            int cond_end      = cond     ? cond->end     : n->pos;
            int whenTrue_pos  = whenTrue ? whenTrue->pos : cond_end;
            int whenTrue_end  = whenTrue ? whenTrue->end : whenTrue_pos;
            int whenFalse_pos = whenFalse ? whenFalse->pos : whenTrue_end;
            int lines_before_q = 0, lines_after_q = 0;
            int lines_before_c = 0, lines_after_c = 0;
            if (e->source) {
                size_t qstart = skip_trivia_u16(e->source, (size_t)cond_end);
                lines_before_q = lines_between_positions(e->source, (size_t)cond_end, qstart) > 0 ? 1 : 0;
                size_t wtstart = skip_trivia_u16(e->source, (size_t)whenTrue_pos);
                lines_after_q  = lines_between_positions(e->source, (size_t)whenTrue_pos, wtstart) > 0 ? 1 : 0;
                size_t cstart = skip_trivia_u16(e->source, (size_t)whenTrue_end);
                lines_before_c = lines_between_positions(e->source, (size_t)whenTrue_end, cstart) > 0 ? 1 : 0;
                size_t wfstart = skip_trivia_u16(e->source, (size_t)whenFalse_pos);
                lines_after_c  = lines_between_positions(e->source, (size_t)whenFalse_pos, wfstart) > 0 ? 1 : 0;
            }
            emit(e, cond);
            write_lines_and_indent(e, lines_before_q, /*writeSpaceIfNotIndenting*/ true);
            write_char(e, '?');
            write_lines_and_indent(e, lines_after_q, /*writeSpaceIfNotIndenting*/ true);
            emit(e, whenTrue);
            decrease_indent_if(e, lines_before_q, lines_after_q);
            write_lines_and_indent(e, lines_before_c, /*writeSpaceIfNotIndenting*/ true);
            write_char(e, ':');
            write_lines_and_indent(e, lines_after_c, /*writeSpaceIfNotIndenting*/ true);
            emit(e, whenFalse);
            decrease_indent_if(e, lines_before_c, lines_after_c);
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitPropertyAccessExpression (~2638): emit the left-hand expression,
         * then the dot token, then the name. ctsc does not yet model
         * preservation of source newlines around the dot; under the
         * transpileModule default (preserveSourceNewlines === undefined) the
         * dot renders inline which matches the fixtures we currently exercise.
         */
        case CTSC_SK_PropertyAccessExpression:
            emit(e, n->data.propertyAccessExpression.expression);
            if (n->data.propertyAccessExpression.optional_chain) {
                write_cstr(e, "?.");
            } else {
                write_char(e, '.');
            }
            emit(e, n->data.propertyAccessExpression.name);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitElementAccessExpression (~2689):
         *     emitExpression(node.expression)
         *     emit(node.questionDotToken)
         *     writePunctuation("[")
         *     emitExpression(node.argumentExpression)
         *     writePunctuation("]")
         * For `new Foo[]` (105_parserObjectCreationArrayLiteral1.ts) the parser
         * synthesises a zero-width missing Identifier as argumentExpression
         * (parser.ts ~6435 createMissingNode); that identifier emits nothing,
         * yielding the expected `Foo[]` byte sequence.
         */
        case CTSC_SK_ElementAccessExpression:
            emit(e, n->data.elementAccessExpression.expression);
            if (n->data.elementAccessExpression.optional_chain) {
                write_cstr(e, "?.");
            }
            write_char(e, '[');
            emit(e, n->data.elementAccessExpression.argumentExpression);
            write_char(e, ']');
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitNewExpression
         * (~2714):
         *     writeKeyword("new");
         *     writeSpace();
         *     emitExpression(node.expression, parenthesizeExpressionOfNew);
         *     emitTypeArguments(node, node.typeArguments);
         *     emitExpressionList(node, node.arguments, ListFormat.NewExpressionArguments, ...);
         * NewExpressionArguments (types.ts ~10169) =
         *     CommaDelimited | SpaceBetweenSiblings | SingleLine | Parenthesis |
         *     OptionalIfUndefined,
         * so the parentheses (and argument list) are omitted when
         * `node.arguments` is undefined (parser.ts parseNewExpressionOrNewDotTarget
         * leaves `arguments === undefined` when the source omits `(...)`, e.g.
         * `new Date`). ctsc models that with has_arguments=false.
         *
         * typeArguments are not yet modelled here — tsc rolls its try-parse
         * back to a relational `<` when the angle-bracket list fails to close
         * (see 105_parserConstructorAmbiguity2.ts: `new Date<A`), so the
         * resulting NewExpression has no typeArguments to emit.
         */
        case CTSC_SK_NewExpression:
            write_cstr(e, "new ");
            emit(e, n->data.newExpression.expression);
            if (n->data.newExpression.has_arguments) {
                write_char(e, '(');
                emit_list(e, &n->data.newExpression.arguments, ", ");
                write_char(e, ')');
            }
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitBlock (~3036)
         * → emitBlockStatements (~3040): the open brace, then emitList with
         * `forceSingleLine = !node.multiLine && isEmptyBlock(node)`, then the
         * close brace. For an empty block the chosen format is
         * ListFormat.SingleLineBlockStatements (types.ts ~10171 =
         * SpaceBetweenBraces | SpaceBetweenSiblings | SingleLine), and
         * emitNodeList (~4700) handles the empty case by writing a single
         * space between the braces because SpaceBetweenBraces is set and
         * NoSpaceIfEmpty is not (~4706). Result: `{ }`.
         *
         * For a non-empty block the format is MultiLineBlockStatements
         * (Indented | MultiLine), which writes a newline + indented
         * statements + a trailing newline before the close brace.
         */
        case CTSC_SK_Block: {
            write_char(e, '{');
            const CtscNodeArray* ss = &n->data.block.statements;
            if (ss->len == 0) {
                /* emitter.ts emitBlock (~3036) passes
                 * `forceSingleLine = !node.multiLine && isEmptyBlock(node)`
                 * to emitBlockStatements (~3040); the format then becomes
                 * SingleLineBlockStatements (SpaceBetweenBraces) or
                 * MultiLineBlockStatements (Indented | MultiLine). In
                 * emitNodeList's empty-list branch (~4701) MultiLine
                 * writes a single `writeLine()` before the closing brace,
                 * while the single-line format writes one space. */
                if (n->data.block.multi_line) {
                    write_line(e);
                } else {
                    write_char(e, ' ');
                }
                write_char(e, '}');
                return;
            }
            e->indent++;
            for (size_t i = 0; i < ss->len; ++i) {
                write_line(e);
                emit(e, ss->items[i]);
            }
            e->indent--;
            write_line(e);
            write_char(e, '}');
            return;
        }
        case CTSC_SK_FunctionDeclaration:
        case CTSC_SK_FunctionExpression: {
            /* Mirrors upstream/TypeScript/src/compiler/emitter.ts
             * emitFunctionDeclarationOrExpression (~3430):
             *     writeKeyword("function");
             *     emit(node.asteriskToken);
             *     writeSpace();
             *     emitIdentifierName(node.name);
             *     emitSignatureAndBody(node, emitSignatureHead, emitFunctionBody);
             * FunctionDeclaration and FunctionExpression share the same
             * printer path (~2755 emitFunctionExpression → ~3430), and their
             * ctsc data shapes are identical (see CtscFunctionDeclarationData
             * in ast.h); only CtscNode.kind distinguishes the two. For a
             * body-less top-level FunctionDeclaration (overload signature),
             * transformers/ts.ts visitFunctionDeclaration (~1555) replaces
             * the node with NotEmittedStatement, which the SourceFile loop
             * already filters via source_file_statement_is_dropped — we
             * simply mirror that here by emitting zero bytes when the body
             * is missing. A body-less FunctionExpression cannot arise from
             * the parser (parseFunctionBlockOrSemicolon always produces a
             * Block in expression position), but the same guard is safe. */
            if (!n->data.functionDeclaration.body) return;
            if (n->kind == CTSC_SK_FunctionDeclaration
                && n->data.functionDeclaration.has_export_default) {
                /* emitter.ts emitExportAssignment (~3734) + function declaration
                 * print: `export default function name() { ... }`. */
                write_cstr(e, "export default ");
            } else if (n->kind == CTSC_SK_FunctionDeclaration
                && n->data.functionDeclaration.has_export) {
                /* emitter.ts emitDecoratorsAndModifiers (~3735) + emitFunctionDeclarationOrExpression
                 * (~3430): ExportKeyword is written before `function`. */
                write_cstr(e, "export ");
            }
            if (n->data.functionDeclaration.has_async) {
                /* emitFunctionDeclarationOrExpression (~3430): AsyncKeyword via emitModifiers. */
                write_cstr(e, "async ");
            }
            write_cstr(e, "function ");
            if (n->data.functionDeclaration.name) emit(e, n->data.functionDeclaration.name);
            write_char(e, '(');
            emit_parameter_list_for_js(e, &n->data.functionDeclaration.parameters);
            write_cstr(e, ") ");
            emit_function_like_block(e, n->data.functionDeclaration.body, false);
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitParameter
         * (~2221):
         *     emitDecoratorsAndModifiers(...);
         *     emit(node.dotDotDotToken);
         *     emitNodeWithWriter(node.name, writeParameter);
         *     emit(node.questionToken);
         *     emitTypeAnnotation(node.type);
         *     emitInitializer(node.initializer, ...);
         * The TS→JS transformer (transformers/ts.ts visitParameter) already
         * drops questionToken and the type annotation before this printer
         * runs, so for JS output we write `...` (when present), the binding
         * name, then ` = initializer` when parse_parameter captured a
         * default (mirrors emitInitializer ~2233).
         */
        case CTSC_SK_Parameter:
            if (n->data.parameter.has_dot_dot_dot) {
                write_cstr(e, "...");
            }
            emit(e, n->data.parameter.name);
            if (n->data.parameter.initializer) {
                write_cstr(e, " = ");
                emit(e, n->data.parameter.initializer);
            }
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitObjectBindingPattern (~2585) / emitArrayBindingPattern (~2591):
         *     writePunctuation('{'); emitList(elements, ObjectBindingPatternElements); writePunctuation('}');
         *     writePunctuation('['); emitList(elements, ArrayBindingPatternElements);  writePunctuation(']');
         * ListFormat.{Object,Array}BindingPatternElements (types.ts ~9025) is
         * SingleLine | AllowTrailingComma | CommaDelimited | SpaceBetweenSiblings
         * (and SpaceBetweenBraces for the object form). For an empty list
         * emitNodeList takes the empty-branch (emitter.ts ~4701) which writes
         * nothing, so `{ }`-less patterns render as a bare `{}` / `[]` — which
         * is what the 107_parserErrorRecovery_ParameterList5.ts fixture's
         * error-recovery second parameter (`{}`) expects.
         */
        case CTSC_SK_ObjectBindingPattern:
            write_char(e, '{');
            emit_list(e, &n->data.bindingPattern.elements, ", ");
            write_char(e, '}');
            return;
        case CTSC_SK_ArrayBindingPattern:
            write_char(e, '[');
            emit_list(e, &n->data.bindingPattern.elements, ", ");
            write_char(e, ']');
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitBindingElement
         * (~2597):
         *     emit(node.dotDotDotToken);
         *     if (node.propertyName) { emit(propertyName); writePunctuation(":");
         *         writeSpace(); }
         *     emit(node.name);
         *     emitInitializer(node.initializer, ...);
         * Type annotations are stripped before print in the real pipeline; ctsc
         * has none on BindingElement. Initializer uses the same ` = ` shape as
         * VariableDeclaration.
         */
        case CTSC_SK_BindingElement:
            if (n->data.bindingElement.has_dotdotdot) {
                write_cstr(e, "...");
            }
            if (n->data.bindingElement.propertyName) {
                emit(e, n->data.bindingElement.propertyName);
                write_char(e, ':');
                write_char(e, ' ');
            }
            if (n->data.bindingElement.name) {
                emit(e, n->data.bindingElement.name);
            }
            if (n->data.bindingElement.initializer) {
                write_cstr(e, " = ");
                emit(e, n->data.bindingElement.initializer);
            }
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitArrowFunction
         * (~2760):
         *     emitModifierList(node, node.modifiers);
         *     emitSignatureAndBody(node, emitArrowFunctionHead, emitArrowFunctionBody);
         * emitArrowFunctionHead (~2765) emits parameters via emitParametersForArrow
         * (~3484), then a space, then the `=>` token. emitParametersForArrow
         * uses ListFormat.Parameters (CommaDelimited | SpaceBetweenSiblings |
         * SingleLine | Parenthesis), which produces `(p1, p2)` — and for an
         * empty list a bare `()`. The head always ends with ` => ` (the space
         * before `=>` comes from writeSpace, the space after comes from
         * emitArrowFunctionBody's writeSpace when the body is an
         * AssignmentExpression, or — when the body is a Block —
         * emitBlockFunctionBody writes its own leading space).
         *
         * emitArrowFunctionBody (~2773):
         *     if (isBlock(node.body)) emitBlockFunctionBody(node.body);
         *     else { writeSpace(); emitExpression(node.body, ...); }
         * For a Block body we mirror ctsc's MethodDeclaration case: prepend a
         * single space before delegating to the Block case, which already
         * renders the braces (empty blocks as `{ }` via SpaceBetweenBraces /
         * SingleLineBlockStatements — utilities.ts ~10171).
         *
         * ctsc does not yet model modifiers / typeParameters / return-type
         * annotation on an ArrowFunction (none of the currently-unlocked
         * fixtures exercise them). Extend this case as those fixtures land.
         */
        case CTSC_SK_ArrowFunction: {
            if (n->data.arrowFunction.has_async) {
                /* emitArrowFunction (~2760): emitModifierList → async */
                write_cstr(e, "async ");
            }
            write_char(e, '(');
            emit_parameter_list_for_js(e, &n->data.arrowFunction.parameters);
            write_cstr(e, ") => ");
            const CtscNode* body = n->data.arrowFunction.body;
            if (body && body->kind == CTSC_SK_Block) {
                emit_function_like_block(e, body, true);
            } else if (body) {
                emit(e, body);
            }
            return;
        }
        case CTSC_SK_ReturnStatement:
            write_cstr(e, "return");
            if (n->data.returnStatement.expression) {
                write_char(e, ' ');
                emit(e, n->data.returnStatement.expression);
            }
            write_char(e, ';');
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitThrowStatement
         * (~3371):
         *     emitTokenWithComment(ThrowKeyword, ...);
         *     emitExpressionWithLeadingSpace(node.expression, ...);
         *     writeTrailingSemicolon();
         */
        case CTSC_SK_ThrowStatement:
            write_cstr(e, "throw");
            write_char(e, ' ');
            emit(e, n->data.throwStatement.expression);
            write_char(e, ';');
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitBreakStatement
         * (~3177) and emitContinueStatement (~3171): emit the keyword, then
         * emitWithLeadingSpace(node.label) — i.e. prefix a single space before
         * the optional label Identifier — followed by writeTrailingSemicolon.
         */
        case CTSC_SK_BreakStatement:
            write_cstr(e, "break");
            if (n->data.breakOrContinueStatement.label) {
                write_char(e, ' ');
                emit(e, n->data.breakOrContinueStatement.label);
            }
            write_char(e, ';');
            return;
        case CTSC_SK_ContinueStatement:
            write_cstr(e, "continue");
            if (n->data.breakOrContinueStatement.label) {
                write_char(e, ' ');
                emit(e, n->data.breakOrContinueStatement.label);
            }
            write_char(e, ';');
            return;
        case CTSC_SK_ParenthesizedExpression:
            write_char(e, '(');
            emit(e, n->data.parenthesizedExpression.expression);
            write_char(e, ')');
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitVoidExpression
         * (~2795), emitDeleteExpression (~2783) and emitTypeOfExpression (~2789):
         *     emitTokenWithComment(<Keyword>, ...); writeSpace();
         *     emitExpression(node.expression, parenthesizer.parenthesizeOperandOfPrefixUnary);
         * `parenthesizeOperandOfPrefixUnary` only injects parens when the
         * operand is itself a BinaryExpression whose operator would otherwise
         * re-associate; for the simple operands we model today (identifiers,
         * literals, property/element access chains) it is a no-op. The three
         * shapes share CtscVoidExpressionData (see ast.h ~336), so the only
         * difference is the keyword text.
         */
        case CTSC_SK_VoidExpression:
            write_cstr(e, "void ");
            emit(e, n->data.voidExpression.expression);
            return;
        case CTSC_SK_AwaitExpression:
            /* emitAwaitExpression (~2801). */
            write_cstr(e, "await ");
            emit(e, n->data.awaitExpression.expression);
            return;
        case CTSC_SK_DeleteExpression:
            write_cstr(e, "delete ");
            emit(e, n->data.voidExpression.expression);
            return;
        case CTSC_SK_TypeOfExpression:
            write_cstr(e, "typeof ");
            emit(e, n->data.voidExpression.expression);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitYieldExpression
         * (~2971):
         *     emitTokenWithComment(YieldKeyword, node.pos, writeKeyword, node);
         *     emit(node.asteriskToken);
         *     emitExpressionWithLeadingSpace(
         *         node.expression && parenthesizeExpressionForNoAsi(node.expression),
         *         parenthesizeExpressionForNoAsiAndDisallowedComma);
         * where emitExpressionWithLeadingSpace (~4554) writes a single space
         * before the operand when it is present, and the `asteriskToken` emit
         * goes through writeTokenNode (~4939) which writes the canonical "*"
         * with no surrounding whitespace. So the three shapes are:
         *     yield
         *     yield <expr>        (has_asterisk=false, expression)
         *     yield*              (has_asterisk=true, no expression — illegal
         *                          but matches tsc's pass-through)
         *     yield* <expr>       (has_asterisk=true, expression)
         * parenthesizeExpressionForNoAsi only matters if the operand starts
         * with a token that would swallow the preceding `yield` under ASI;
         * for the leaf operands we currently parse (identifiers, numeric
         * literals, string literals) it is a no-op.
         */
        case CTSC_SK_YieldExpression:
            write_cstr(e, "yield");
            if (n->data.yieldExpression.has_asterisk) write_char(e, '*');
            if (n->data.yieldExpression.expression) {
                write_char(e, ' ');
                emit(e, n->data.yieldExpression.expression);
            }
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitPrefixUnaryExpression
         * (~2807): write the operator token, then optionally a space, then the operand.
         * shouldEmitWhitespaceBeforeOperand (~2815) inserts a space only when the
         * operand is itself a PrefixUnaryExpression AND the outer/inner operators
         * would otherwise fuse into a different token (e.g. `+ +x`, `+ ++x`,
         * `- -x`, `- --x`). For all other operand shapes (including
         * ObjectLiteralExpression as in `++{}`) no separator is emitted.
         */
        case CTSC_SK_PrefixUnaryExpression: {
            CtscSyntaxKind op = n->data.prefixUnaryExpression.operator_kind;
            const CtscNode* operand = n->data.prefixUnaryExpression.operand;
            write_cstr(e, op_text(op));
            if (operand && operand->kind == CTSC_SK_PrefixUnaryExpression) {
                CtscSyntaxKind inner = operand->data.prefixUnaryExpression.operator_kind;
                bool fuse_plus  = (op == CTSC_SK_PlusToken)
                    && (inner == CTSC_SK_PlusToken || inner == CTSC_SK_PlusPlusToken);
                bool fuse_minus = (op == CTSC_SK_MinusToken)
                    && (inner == CTSC_SK_MinusToken || inner == CTSC_SK_MinusMinusToken);
                if (fuse_plus || fuse_minus) write_char(e, ' ');
            }
            emit(e, operand);
            return;
        }
        /*
         * Mirrors upstream emitter.ts emitPostfixUnaryExpression (~2834):
         * operand followed by the operator token, with no intervening space.
         */
        case CTSC_SK_PostfixUnaryExpression:
            emit(e, n->data.postfixUnaryExpression.operand);
            write_cstr(e, op_text(n->data.postfixUnaryExpression.operator_kind));
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitObjectLiteralExpression (~2618):
         *     writePunctuation("{");
         *     emitList(node, node.properties, ObjectLiteralExpressionProperties)
         *     writePunctuation("}");
         * ObjectLiteralExpressionProperties (types.ts ~10163) =
         *     PreserveLines | CommaDelimited | SpaceBetweenSiblings |
         *     SpaceBetweenBraces | Indented | Braces | NoSpaceIfEmpty.
         * For a single-line literal with ≥1 property, the list format expands
         * to "{ p1, p2 }"; an empty properties list collapses to "{}" via
         * NoSpaceIfEmpty (utilities.ts emitList ~4706). When `multi_line` is
         * true (parser ~6759), PreferNewLine (~2627) emits each property on
         * its own indented line with commas per `has_trailing_comma`.
         *
         * The ts.transpileModule pipeline runs the declaration transformer
         * before the printer, which drops body-less MethodDeclarations
         * synthesized by error recovery (see parseFunctionBlockOrSemicolon
         * ~7567: when there is neither `{` nor a semicolon-able position,
         * body is left undefined). Mirror that by skipping
         * MethodDeclarations with a NULL body when computing the emitted
         * property list; the 106_FunctionPropertyAssignments4_es6.ts
         * fixture (`var v = { * }`) exercises that path and expects `{}`.
         */
        case CTSC_SK_ObjectLiteralExpression: {
            const CtscNodeArray* props = &n->data.objectLiteralExpression.properties;
            bool multi_line = n->data.objectLiteralExpression.multi_line;
            bool has_trailing_comma = n->data.objectLiteralExpression.has_trailing_comma;
            size_t emit_count = 0;
            for (size_t i = 0; i < props->len; ++i) {
                const CtscNode* prop = props->items[i];
                if (prop->kind == CTSC_SK_MethodDeclaration
                    && prop->data.methodDeclaration.body == NULL) continue;
                if ((prop->kind == CTSC_SK_GetAccessor || prop->kind == CTSC_SK_SetAccessor)
                    && prop->data.accessorDeclaration.body == NULL) continue;
                emit_count++;
            }
            write_char(e, '{');
            if (emit_count == 0) {
                write_char(e, '}');
                return;
            }
            if (!multi_line) {
                write_char(e, ' ');
                bool first = true;
                for (size_t i = 0; i < props->len; ++i) {
                    const CtscNode* prop = props->items[i];
                    if (prop->kind == CTSC_SK_MethodDeclaration
                        && prop->data.methodDeclaration.body == NULL) continue;
                    if ((prop->kind == CTSC_SK_GetAccessor || prop->kind == CTSC_SK_SetAccessor)
                        && prop->data.accessorDeclaration.body == NULL) continue;
                    if (!first) write_cstr(e, ", ");
                    first = false;
                    emit(e, prop);
                }
                write_char(e, ' ');
                write_char(e, '}');
                return;
            }
            e->indent++;
            size_t seen = 0;
            for (size_t i = 0; i < props->len; ++i) {
                const CtscNode* prop = props->items[i];
                if (prop->kind == CTSC_SK_MethodDeclaration
                    && prop->data.methodDeclaration.body == NULL) continue;
                if ((prop->kind == CTSC_SK_GetAccessor || prop->kind == CTSC_SK_SetAccessor)
                    && prop->data.accessorDeclaration.body == NULL) continue;
                write_line(e);
                emit(e, prop);
                seen++;
                bool is_last = (seen == emit_count);
                if (!is_last || has_trailing_comma) write_char(e, ',');
            }
            e->indent--;
            write_line(e);
            write_char(e, '}');
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitAccessorDeclaration (~2291):
         *     emitDecoratorsAndModifiers(...);
         *     writeKeyword("get"|"set"); writeSpace(); emit(node.name); ...
         */
        case CTSC_SK_GetAccessor:
        case CTSC_SK_SetAccessor: {
            emit_class_element_modifiers_for_js(e, &n->data.accessorDeclaration.modifiers);
            write_cstr(e, n->kind == CTSC_SK_GetAccessor ? "get " : "set ");
            if (n->data.accessorDeclaration.name) emit(e, n->data.accessorDeclaration.name);
            write_char(e, '(');
            emit_parameter_list_for_js(e, &n->data.accessorDeclaration.parameters);
            write_char(e, ')');
            if (n->data.accessorDeclaration.body) {
                write_char(e, ' ');
                emit_function_like_block(e, n->data.accessorDeclaration.body, false);
            }
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitMethodDeclaration (~2270):
         *     emit(node.asteriskToken);
         *     emit(node.name);
         *     emit(node.questionToken);
         *     emitSignatureAndBody(node, emitSignatureHead, emitFunctionBody);
         * emitSignatureHead (~3470) calls emitParameters(...) which is
         * `emitList(... ListFormat.Parameters)` — CommaDelimited |
         * SpaceBetweenSiblings | SingleLine | Parenthesis (types.ts
         * ~10169). For an empty parameter list that yields `()`.
         *
         * emitFunctionBody (~3456) delegates to emitBlockFunctionBody
         * (~3516) which writes a leading space, `{`, then the body's
         * statements on a single line when the block is empty or already
         * marked single-line (shouldEmitBlockFunctionBodyOnSingleLine
         * ~3476), then `}`. For the 106_FunctionPropertyAssignments3_es6.ts
         * fixture the body is an empty Block on the same source line,
         * so the single-line path fires and tsc writes ` { }`.
         *
         * ctsc's Block case already renders empty blocks as `{ }` with an
         * internal space; we just need to prepend the separating space
         * before delegating. body-less MethodDeclarations (no `{` AND a
         * semicolon-able position, e.g. `var v = { * }`) are filtered out
         * by the caller in ObjectLiteralExpression above — matching tsc's
         * transpileModule pipeline which strips such declarations during
         * transformation.
         */
        case CTSC_SK_MethodDeclaration: {
            emit_class_element_modifiers_for_js(e, &n->data.methodDeclaration.modifiers);
            if (n->data.methodDeclaration.has_asterisk) write_char(e, '*');
            if (n->data.methodDeclaration.name) emit(e, n->data.methodDeclaration.name);
            write_char(e, '(');
            emit_parameter_list_for_js(e, &n->data.methodDeclaration.parameters);
            write_char(e, ')');
            if (n->data.methodDeclaration.body) {
                write_char(e, ' ');
                emit_function_like_block(e, n->data.methodDeclaration.body, false);
            }
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitArrayLiteralExpression (~2612):
         *     emitExpressionList(node, elements,
         *         ListFormat.ArrayLiteralExpressionElements | preferNewLine,
         *         parenthesizer.parenthesizeExpressionForDisallowedComma);
         * where ArrayLiteralExpressionElements (types.ts ~10166) =
         *     PreserveLines | CommaDelimited | SpaceBetweenSiblings |
         *     AllowTrailingComma | Indented | SquareBrackets.
         *
         * For a single-line array with a handful of elements (the common case,
         * and the only one that 105_parserUnaryExpression3.ts — `++[0];` —
         * exercises), emitNodeList (~4700) degenerates to: write "[",
         * iterate elements writing ", " between them, write "]". The
         * PreserveLines / Indented flags only take effect when the source
         * range spans multiple lines, which we do not currently model.
         *
         * ctsc's parser maps consecutive commas (tsc's OmittedExpression) by
         * just advancing past them (parser.c parse_primary ~189), so we never
         * produce a literal hole here. SpreadElement is parsed via
         * parse_argument_expression (~6685).
         *
         * A trailing comma on the source element list (NodeArray.hasTrailingComma,
         * parser.ts ~3496) round-trips through emitNodeListItems' trailing-comma
         * branch (emitter.ts ~4824): when hasTrailingComma &&
         * (format & AllowTrailingComma) && (format & CommaDelimited), a bare
         * `,` is written after the last element with no separating space.
         * ArrayLiteralExpressionElements (types.ts ~10166) sets both flags, so
         * `[1, 1,]` emits as `[1, 1,]` — see parserArrayLiteralExpression10.ts.
         */
        case CTSC_SK_ArrayLiteralExpression: {
            write_char(e, '[');
            const CtscNodeArray* elems = &n->data.arrayLiteralExpression.elements;
            for (size_t i = 0; i < elems->len; ++i) {
                if (i) write_cstr(e, ", ");
                emit(e, elems->items[i]);
            }
            if (elems->len > 0 && n->data.arrayLiteralExpression.has_trailing_comma) {
                write_char(e, ',');
            }
            write_char(e, ']');
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitClassDeclarationOrExpression (~3557):
         *     emitDecoratorsAndModifiers(...);
         *     emitTokenWithComment(ClassKeyword, ...);         // "class"
         *     if (node.name) { writeSpace(); emitIdentifierName(node.name); }
         *     emitTypeParameters(...);
         *     emitList(node, heritageClauses, ClassHeritageClauses);
         *     writeSpace();
         *     writePunctuation("{");
         *     emitList(node, members, ListFormat.ClassMembers);
         *     writePunctuation("}");
         * ClassMembers (types.ts ~10177) = Indented | MultiLine. For an
         * empty member list emitNodeList (~4700) falls into the isEmpty
         * branch and writes a single `writeLine()` because MultiLine is
         * set — the writer collapses consecutive line starts, so back-to-
         * back `{` + writeLine yields exactly one `\n` before `}`.
         *
         * For the 106_parser512084.ts fixture (`// @target: es2015\r\nclass foo {\r\n`)
         * the parser produces a ClassDeclaration with name=foo and an
         * empty members NodeArray (the body is unterminated — the parser
         * reports "'}' expected." but still finishes the node, matching
         * tsc's error-recovery path). Emitting it yields
         *     class foo {
         *     }
         * which is what the transpileModule oracle prints.
         *
         * Modifiers / typeParameters are still partial; heritageClauses are
         * emitted (emit_class_heritage_clauses) to match ClassHeritageClauses.
         */
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitPropertyAssignment (~4052):
         *     emit(node.name);
         *     writePunctuation(":");
         *     writeSpace();
         *     emitExpression(initializer, parenthesizer.parenthesizeExpressionForDisallowedComma);
         * The trailing comment replay for the initializer
         * (emitTrailingCommentsOfPosition ~4066) is not yet modelled — the
         * transpileModule pipeline emits no trailing comments for the
         * fixtures we currently have. When the colon is missing in source
         * (e.g. `{ [e] }`, parserComputedPropertyName1.ts), the parser
         * synthesises a zero-width missing Identifier for `initializer` —
         * its empty text_len makes write_u16 a no-op, so the emitted bytes
         * become `name: ` (trailing space and nothing after), matching tsc.
         */
        case CTSC_SK_PropertyAssignment:
            emit(e, n->data.propertyAssignment.name);
            write_char(e, ':');
            write_char(e, ' ');
            emit(e, n->data.propertyAssignment.initializer);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitShorthandPropertyAssignment (~4071):
         *     emit(node.name);
         *     if (node.objectAssignmentInitializer) {
         *         writeSpace(); writePunctuation("="); writeSpace();
         *         emitExpression(node.objectAssignmentInitializer, ...);
         *     }
         * parser builds these with name=Identifier, and
         * objectAssignmentInitializer only when the source had `= expr`.
         */
        case CTSC_SK_ShorthandPropertyAssignment:
            emit(e, n->data.shorthandPropertyAssignment.name);
            if (n->data.shorthandPropertyAssignment.objectAssignmentInitializer) {
                write_cstr(e, " = ");
                emit(e, n->data.shorthandPropertyAssignment.objectAssignmentInitializer);
            }
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitSpreadAssignment (~4081):
         *     emitTokenWithComment(DotDotDotToken, ...);
         *     emitExpression(node.expression, ...);
         */
        case CTSC_SK_SpreadAssignment:
            write_cstr(e, "...");
            emit(e, n->data.spreadAssignment.expression);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitSpreadExpression (~4071): DotDotDotToken + expression.
         */
        case CTSC_SK_SpreadElement:
            write_cstr(e, "...");
            emit(e, n->data.spreadElement.expression);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitComputedPropertyName (~2194):
         *     writePunctuation("[");
         *     emitExpression(node.expression, parenthesizer.parenthesizeExpressionOfComputedPropertyName);
         *     writePunctuation("]");
         * parenthesizeExpressionOfComputedPropertyName wraps a CommaList /
         * binary-comma expression in parens; for the leaf expressions we
         * currently parse (Identifier, literals) it is a no-op.
         */
        case CTSC_SK_ComputedPropertyName:
            write_char(e, '[');
            emit(e, n->data.computedPropertyName.expression);
            write_char(e, ']');
            return;
        /*
         * ClassExpression and ClassDeclaration share the upstream printer
         * (emitter.ts emitClassDeclarationOrExpression ~3557), and ctsc
         * stores both in CtscClassDeclarationData (see ast.h ~461). The
         * 107_classExpressionES61.ts fixture (`var v = class C {};`) lands
         * here via parse_primary's ClassKeyword branch, which builds a
         * CTSC_SK_ClassExpression with the same shape as the declaration
         * form — so we fall through to the same emit path.
         */
        case CTSC_SK_ClassDeclaration:
        case CTSC_SK_ClassExpression: {
            const CtscClassDeclarationData* cd = &n->data.classDeclaration;
            const CtscNodeArray* ms = &cd->members;

            const CtscNode* inst_prop_buf[128];
            const CtscNode* static_prop_buf[128];
            size_t n_inst = 0;
            size_t n_stat = 0;
            for (size_t i = 0; i < ms->len; ++i) {
                const CtscNode* mem = ms->items[i];
                if (mem->kind != CTSC_SK_PropertyDeclaration
                    || !mem->data.propertyDeclaration.initializer) {
                    continue;
                }
                if (property_declaration_is_static(mem)) {
                    if (n_stat < 128) static_prop_buf[n_stat++] = mem;
                } else {
                    if (n_inst < 128) inst_prop_buf[n_inst++] = mem;
                }
            }

            if (n_inst == 0) {
                if (n->kind == CTSC_SK_ClassDeclaration && cd->has_export) {
                    write_cstr(e, "export ");
                }
                write_cstr(e, "class");
                if (cd->name) {
                    write_char(e, ' ');
                    emit(e, cd->name);
                }
                emit_class_heritage_clauses(e, cd);
                write_char(e, ' ');
                write_char(e, '{');
                /*
                 * Mirrors upstream/TypeScript/src/compiler/transformers/ts.ts
                 * visitMethodDeclaration (~1473): a MethodDeclaration whose body
                 * is missing (overload signature, or parse-error recovery such
                 * as 107_MemberFunctionDeclaration5_es6.ts `class C {\n *\n}`)
                 * is dropped by the TS-to-JS transformer via
                 * shouldEmitFunctionLikeDeclaration (~1297 `!nodeIsMissing(body)`).
                 * The printer then sees a class with zero members and emits
                 * `class C {\n}`. We filter body-less MethodDeclarations out of
                 * the emitted members list to match that, using the same shape
                 * already applied to ObjectLiteralExpression properties above.
                 */
                size_t emit_count = 0;
                for (size_t i = 0; i < ms->len; ++i) {
                    const CtscNode* m = ms->items[i];
                    if (class_member_is_body_less_function_like(m)) continue;
                    if (m->kind == CTSC_SK_PropertyDeclaration
                        && m->data.propertyDeclaration.initializer) {
                        continue;
                    }
                    emit_count++;
                }
                if (emit_count == 0) {
                    write_line(e);
                } else {
                    e->indent++;
                    for (size_t i = 0; i < ms->len; ++i) {
                        const CtscNode* m = ms->items[i];
                        if (class_member_is_body_less_function_like(m)) continue;
                        if (m->kind == CTSC_SK_PropertyDeclaration
                            && m->data.propertyDeclaration.initializer) {
                            continue;
                        }
                        write_line(e);
                        if (method_is_constructor(m)) {
                            emit_merged_constructor_body(
                                e, inst_prop_buf, 0, m, heritage_has_extends(cd));
                        } else {
                            emit(e, m);
                        }
                    }
                    e->indent--;
                    write_line(e);
                }
                write_char(e, '}');
                emit_trailing_static_property_initializers(e, cd, static_prop_buf, n_stat);
                return;
            }

            bool has_ext = heritage_has_extends(cd);
            const CtscNode* ctor_m = NULL;
            for (size_t i = 0; i < ms->len; ++i) {
                const CtscNode* mem = ms->items[i];
                if (method_is_constructor(mem) && mem->data.methodDeclaration.body) {
                    ctor_m = mem;
                    break;
                }
            }

            if (n->kind == CTSC_SK_ClassDeclaration && cd->has_export) {
                write_cstr(e, "export ");
            }
            write_cstr(e, "class");
            if (cd->name) {
                write_char(e, ' ');
                emit(e, cd->name);
            }
            emit_class_heritage_clauses(e, cd);
            write_char(e, ' ');
            write_char(e, '{');
            e->indent++;
            if (!ctor_m) {
                write_line(e);
                emit_synthetic_constructor_for_property_inits(e, inst_prop_buf, n_inst);
            }
            for (size_t i = 0; i < ms->len; ++i) {
                const CtscNode* mem = ms->items[i];
                if (mem->kind == CTSC_SK_PropertyDeclaration
                    && mem->data.propertyDeclaration.initializer) {
                    continue;
                }
                if (class_member_is_body_less_function_like(mem)) {
                    continue;
                }
                if (method_is_constructor(mem)) {
                    write_line(e);
                    emit_merged_constructor_body(e, inst_prop_buf, n_inst, mem, has_ext);
                    continue;
                }
                write_line(e);
                emit(e, mem);
            }
            e->indent--;
            write_line(e);
            write_char(e, '}');
            emit_trailing_static_property_initializers(e, cd, static_prop_buf, n_stat);
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/transformers/ts.ts
         * visitEnumDeclaration (~1802) + transformEnumBody (~1891) +
         * transformEnumMember (~1913), plus the printer output of the
         * synthesised VariableStatement (addVarForEnumOrModuleDeclaration
         * ~2018) and the trailing IIFE ExpressionStatement.
         *
         * The lowering produces two source-order statements:
         *     var <name>;
         *     (function (<name>) {
         *         <member-assignments...>
         *     })(<name> || (<name> = {}));
         *
         * At SourceFile lexical scope (our only case today) the var flag is
         * NodeFlags.None — hence `var` instead of `let`. Emitting the two
         * statements inside this single emitter case is byte-compatible with
         * the upstream pipeline because the parent SourceFile loop appends a
         * trailing '\n' to each statement, and the internal separator we
         * write here (`write_line`) collapses against that tail via the
         * text writer's `if (!lineStart) \n` rule (utilities.ts ~6366).
         *
         * For each EnumMember stored as a PropertyAssignment the emitted
         * member statement follows transformEnumMember's two shapes:
         *   - string (or syntactically string) initializer:
         *       <name>["<key>"] = <value>;
         *   - any other (numeric / computed / undefined) initializer:
         *       <name>[<name>["<key>"] = <value>] = "<key>";
         * For an undefined initializer (bare `A,` in source) the checker
         * supplies the next auto-increment constant (transformers/ts.ts
         * transformEnumMemberDeclarationValue ~1952): ctsc tracks the same
         * running counter in the emitter (parse_enum_member leaves
         * `initializer` NULL when `= expr` was omitted).
         */
        case CTSC_SK_EnumDeclaration: {
            const CtscNode* name = n->data.enumDeclaration.name;
            const CtscNodeArray* members = &n->data.enumDeclaration.members;

            /* Statement 1: `var <name>;` — `export enum` lowers to `export var`
             * (addVarForEnumOrModuleDeclaration preserves ExportKeyword on the
             * synthesised VariableStatement; upstream emitter.ts prints it). */
            if (n->data.enumDeclaration.has_export) {
                write_cstr(e, "export ");
            }
            write_cstr(e, "var ");
            if (name) emit(e, name);
            write_char(e, ';');

            /* Separator between the two synthesised top-level statements —
             * the outer SourceFile loop will still append its own trailing
             * newline after the whole EnumDeclaration node. */
            write_line(e);

            /* Statement 2: `(function (<name>) {\n<body>\n})(<name> || (<name> = {}));`
             * ParenthesizeLeftSideOfAccess (parenthesizer.ts) wraps a
             * FunctionExpression in parens when it appears as the callee of
             * a CallExpression, so we explicitly emit the surrounding `(`/`)`.
             */
            write_char(e, '(');
            write_cstr(e, "function ");
            write_char(e, '(');
            if (name) emit(e, name);
            write_cstr(e, ") {");
            if (members->len > 0) {
                e->indent++;
                int64_t enum_auto = 0;
                for (size_t i = 0; i < members->len; ++i) {
                    write_line(e);
                    const CtscNode* m = members->items[i];
                    /* EnumMember uses the PropertyAssignment union slot (see
                     * parser.c parse_enum_member). PropertyAssignment is kept
                     * for symmetry; emitted shape follows transformEnumMember
                     * (~1913). */
                    if (m->kind == CTSC_SK_PropertyAssignment
                        || m->kind == CTSC_SK_EnumMember) {
                        const CtscNode* mname = m->data.propertyAssignment.name;
                        const CtscNode* init  = m->data.propertyAssignment.initializer;
                        bool is_string_init = init && init->kind == CTSC_SK_StringLiteral;
                        if (is_string_init) {
                            /* <name>[<key>] = <value>; */
                            if (name) emit(e, name);
                            write_char(e, '[');
                            emit_enum_property_name_expression(e, mname);
                            write_cstr(e, "] = ");
                            emit(e, init);
                            write_char(e, ';');
                        } else {
                            /* <name>[<name>[<key>] = <value>] = <key>; */
                            if (name) emit(e, name);
                            write_char(e, '[');
                            if (name) emit(e, name);
                            write_char(e, '[');
                            emit_enum_property_name_expression(e, mname);
                            write_cstr(e, "] = ");
                            emit_enum_member_value_and_bump_auto(e, init, &enum_auto);
                            write_cstr(e, "] = ");
                            emit_enum_property_name_expression(e, mname);
                            write_char(e, ';');
                        }
                    } else {
                        emit(e, m);
                    }
                }
                e->indent--;
                write_line(e);
            } else {
                /* Empty enum body: transformEnumBody still creates a
                 * Block with multiLine=true (factory.createBlock(..., true)),
                 * so the printer writes `{\n}` even with no members. */
                write_line(e);
            }
            write_cstr(e, "})(");
            if (name) emit(e, name);
            write_cstr(e, " || (");
            if (name) emit(e, name);
            write_cstr(e, " = {}));");
            return;
        }
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitExpressionWithTypeArguments (~2987):
         *     emitExpression(node.expression, ...);
         *     emitTypeArguments(node, node.typeArguments);
         * JS output omits type arguments; heritage / `new` uses the same shape.
         */
        case CTSC_SK_ExpressionWithTypeArguments:
            emit(e, n->data.expressionWithTypeArguments.expression);
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitImportDeclaration
         * (~3688), emitImportClause (~3705), emitNamedImports (~3726),
         * emitImportSpecifier (~3730).
         */
        case CTSC_SK_ImportDeclaration:
            write_cstr(e, "import ");
            if (n->data.importDeclaration.importClause) {
                emit_import_clause_js(e, n->data.importDeclaration.importClause);
                write_cstr(e, " from ");
            }
            emit(e, n->data.importDeclaration.moduleSpecifier);
            write_char(e, ';');
            return;
        case CTSC_SK_ImportClause:
            emit_import_clause_js(e, n);
            return;
        case CTSC_SK_NamedImports: {
            const CtscNodeArray* el = &n->data.namedImports.elements;
            if (el->len == 0) {
                write_cstr(e, "{}");
            } else {
                write_cstr(e, "{ ");
                emit_list(e, el, ", ");
                write_cstr(e, " }");
            }
            return;
        }
        case CTSC_SK_NamespaceImport:
            write_char(e, '*');
            write_cstr(e, " as ");
            emit(e, n->data.namespaceImport.name);
            return;
        case CTSC_SK_ImportSpecifier:
            if (n->data.importSpecifier.propertyName) {
                emit(e, n->data.importSpecifier.propertyName);
                write_cstr(e, " as ");
            }
            emit(e, n->data.importSpecifier.name);
            return;
        /*
         * emitExportDeclaration (~3753); `export * as ns from` lowers via
         * transformers/module/esnextAnd2015.ts visitExportDeclaration (~331)
         * when module kind is ES2015 (transpileModule default).
         */
        case CTSC_SK_ExportDeclaration: {
            const CtscExportDeclarationData* d = &n->data.exportDeclaration;
            if (d->export_clause && d->export_clause->kind == CTSC_SK_NamespaceExport && d->module_specifier) {
                const CtscNode* export_name = d->export_clause->data.namespaceExport.name;
                write_cstr(e, "import * as ");
                write_u16(e, export_name->data.identifier.text, export_name->data.identifier.text_len);
                write_cstr(e, "_1 from ");
                emit(e, d->module_specifier);
                write_cstr(e, ";\nexport { ");
                write_u16(e, export_name->data.identifier.text, export_name->data.identifier.text_len);
                write_cstr(e, "_1 as ");
                emit(e, export_name);
                write_cstr(e, " };");
                return;
            }
            write_cstr(e, "export ");
            if (d->export_clause) {
                emit(e, d->export_clause);
            } else {
                write_cstr(e, "*");
            }
            if (d->module_specifier) {
                write_cstr(e, " from ");
                emit(e, d->module_specifier);
            }
            write_char(e, ';');
            return;
        }
        case CTSC_SK_NamedExports: {
            const CtscNodeArray* el = &n->data.namedExports.elements;
            if (el->len == 0) {
                write_cstr(e, "{}");
            } else {
                write_cstr(e, "{ ");
                emit_list(e, el, ", ");
                write_cstr(e, " }");
            }
            return;
        }
        case CTSC_SK_ExportSpecifier:
            if (n->data.exportSpecifier.propertyName) {
                emit(e, n->data.exportSpecifier.propertyName);
                write_cstr(e, " as ");
            }
            emit(e, n->data.exportSpecifier.name);
            return;
        case CTSC_SK_NamespaceExport:
            write_char(e, '*');
            write_cstr(e, " as ");
            emit(e, n->data.namespaceExport.name);
            return;
        default:
            return;
    }
}

/*
 * Mirrors transformers/ts.ts visitNamedImportBindings / visitImportSpecifier
 * (~2301-2320): emit only value specifiers (strip `type` in `{ value, type T }`).
 */
static void emit_named_import_bindings_value_only(Emitter* e, const CtscNode* nb) {
    if (nb->kind == CTSC_SK_NamespaceImport) {
        emit(e, nb);
        return;
    }
    const CtscNodeArray* el = &nb->data.namedImports.elements;
    size_t count = 0;
    for (size_t i = 0; i < el->len; i++) {
        CtscNode* sp = el->items[i];
        if (sp->kind == CTSC_SK_ImportSpecifier && !sp->data.importSpecifier.is_type_only) count++;
    }
    if (count == 0) {
        write_cstr(e, "{}");
        return;
    }
    write_cstr(e, "{ ");
    bool first = true;
    for (size_t i = 0; i < el->len; i++) {
        CtscNode* sp = el->items[i];
        if (sp->kind != CTSC_SK_ImportSpecifier || sp->data.importSpecifier.is_type_only) continue;
        if (!first) write_cstr(e, ", ");
        first = false;
        emit(e, sp);
    }
    write_cstr(e, " }");
}

/* Mirrors emitter.ts emitImportClause (~3705) after the elision transformer. */
static void emit_import_clause_js(Emitter* e, const CtscNode* clause) {
    const CtscImportClauseData* c = &clause->data.importClause;
    if (c->name) {
        emit(e, c->name);
        if (c->namedBindings) {
            if (c->namedBindings->kind == CTSC_SK_NamedImports) {
                size_t vc = 0;
                const CtscNodeArray* el = &c->namedBindings->data.namedImports.elements;
                for (size_t j = 0; j < el->len; j++) {
                    CtscNode* sp = el->items[j];
                    if (sp->kind == CTSC_SK_ImportSpecifier && !sp->data.importSpecifier.is_type_only) vc++;
                }
                if (vc > 0) {
                    write_cstr(e, ", ");
                    emit_named_import_bindings_value_only(e, c->namedBindings);
                }
            } else {
                write_cstr(e, ", ");
                emit(e, c->namedBindings);
            }
        }
    } else if (c->namedBindings) {
        emit_named_import_bindings_value_only(e, c->namedBindings);
    }
}

void ctsc_emit_js(const CtscNode* sourceFile, const CtscUtf16Buf* source, CtscBuffer* out) {
    Emitter e = { .out = out, .source = source, .indent = 0, .line_start = true };
    emit(&e, sourceFile);
}
