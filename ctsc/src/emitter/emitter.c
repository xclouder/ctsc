#include "ctsc/emitter.h"
#include "ctsc/ast.h"
#include "ctsc/scanner.h"
#include "ctsc/buffer.h"
#include "ctsc/utf8.h"

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

/*
 * Mirrors upstream/TypeScript/src/compiler/emitter.ts getLinesBetweenNodes
 * (~5196): without preserveSourceNewlines (the transpileModule default) tsc
 * returns `rangeEndIsOnSameLineAsRangeStart(node1, node2) ? 0 : 1`, where
 * the start of range2 is taken through skipTrivia (utilities.ts ~7965).
 * That is sufficient for the cascade in 105_parserGreaterThanTokenAmbiguity4.
 */
static void emit(Emitter* e, const CtscNode* n);

static void emit_list(Emitter* e, const CtscNodeArray* arr, const char* sep) {
    for (size_t i = 0; i < arr->len; ++i) {
        if (i) write_cstr(e, sep);
        emit(e, arr->items[i]);
    }
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

    while (pos < len) {
        uint16_t c = t[pos];
        if (c == 0x0D) {
            if (pos + 1 < len && t[pos + 1] == 0x0A) pos += 2; else pos++;
            continue;
        }
        if (c == 0x0A || c == 0x2028 || c == 0x2029) { pos++; continue; }
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
            write_u16(e, t + start, pos - start);
            if (has_trailing_nl) {
                write_raw_newline(e);
            } else if (is_block) {
                write_char(e, ' ');
            }
            continue;
        }
        break;
    }
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
            size_t leading_pos = ss->len > 0
                ? (size_t)ss->items[0]->pos
                : (size_t)n->data.sourceFile.statements_end;
            emit_source_file_leading_comments(e, leading_pos);
            for (size_t i = 0; i < ss->len; ++i) {
                emit(e, ss->items[i]);
                write_raw_newline(e);
            }
            return;
        }
        case CTSC_SK_EmptyStatement:
            write_char(e, ';');
            return;
        /*
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts emitDebuggerStatement
         * (~3393): writeToken(DebuggerKeyword) + writeTrailingSemicolon.
         */
        case CTSC_SK_DebuggerStatement:
            write_cstr(e, "debugger;");
            return;
        case CTSC_SK_ExpressionStatement:
            emit(e, n->data.expressionStatement.expression);
            write_char(e, ';');
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
            /* Mirrors upstream/TypeScript/src/compiler/emitter.ts emitLiteral
             * (~2118) → getLiteralText (utilities.ts ~1980): for a terminated
             * NoSubstitutionTemplateLiteral with a parent, canUseOriginalText
             * (~2036) returns true, so the emitter writes the on-disk lexeme
             * verbatim (including surrounding backticks). The parser stores
             * that lexeme on templateLiteralLike.text (scanner token slice).
             * Unterminated literals still have the leading backtick captured
             * in `text` — tsc's behaviour for unterminated templates is the
             * same write-back (isUnterminated doesn't force escaping here
             * because flags & TerminateUnterminatedLiterals is not set for
             * the JS emitter). */
            write_u16(e, n->data.templateLiteralLike.text, n->data.templateLiteralLike.text_len);
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
            /* tsc emitter re-escapes based on `singleQuote`; keep original
             * quoting style and decoded value. Use double quote by default. */
            char q = n->data.stringLiteral.single_quote ? '\'' : '"';
            write_char(e, q);
            /* naive: assume value has no quotes/backslashes for now; the
             * fixtures we currently have ("s", "hello") are fine. */
            write_u16(e, n->data.stringLiteral.value, n->data.stringLiteral.value_len);
            write_char(e, q);
            return;
        }
        case CTSC_SK_VariableStatement:
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
            write_lines_and_indent(e, lines_before, /*writeSpaceIfNotIndenting*/ true);
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
         * Mirrors upstream/TypeScript/src/compiler/emitter.ts
         * emitPropertyAccessExpression (~2638): emit the left-hand expression,
         * then the dot token, then the name. ctsc does not yet model
         * preservation of source newlines around the dot; under the
         * transpileModule default (preserveSourceNewlines === undefined) the
         * dot renders inline which matches the fixtures we currently exercise.
         */
        case CTSC_SK_PropertyAccessExpression:
            emit(e, n->data.propertyAccessExpression.expression);
            write_char(e, '.');
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
            if (ss->len == 0) { write_char(e, ' '); write_char(e, '}'); return; }
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
        case CTSC_SK_FunctionDeclaration: {
            write_cstr(e, "function ");
            if (n->data.functionDeclaration.name) emit(e, n->data.functionDeclaration.name);
            write_char(e, '(');
            emit_list(e, &n->data.functionDeclaration.parameters, ", ");
            write_cstr(e, ") ");
            if (n->data.functionDeclaration.body) emit(e, n->data.functionDeclaration.body);
            return;
        }
        case CTSC_SK_Parameter:
            emit(e, n->data.parameter.name);
            return;
        case CTSC_SK_ReturnStatement:
            write_cstr(e, "return");
            if (n->data.returnStatement.expression) {
                write_char(e, ' ');
                emit(e, n->data.returnStatement.expression);
            }
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
         * Mirrors upstream emitter.ts emitObjectLiteralExpression (~2618):
         * "{" ObjectLiteralElement (, ObjectLiteralElement)* "}". ctsc does not
         * yet model object members, so we emit only the braces (matches tsc for
         * empty literals, e.g. `++{}` from parserUnaryExpression4).
         */
        case CTSC_SK_ObjectLiteralExpression: {
            write_char(e, '{');
            const CtscNodeArray* props = &n->data.objectLiteralExpression.properties;
            for (size_t i = 0; i < props->len; ++i) {
                if (i) write_cstr(e, ", ");
                emit(e, props->items[i]);
            }
            write_char(e, '}');
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
         * produce a literal hole here. Trailing commas and SpreadElement are
         * not yet modelled either; fixtures will grow them when needed.
         */
        case CTSC_SK_ArrayLiteralExpression: {
            write_char(e, '[');
            const CtscNodeArray* elems = &n->data.arrayLiteralExpression.elements;
            for (size_t i = 0; i < elems->len; ++i) {
                if (i) write_cstr(e, ", ");
                emit(e, elems->items[i]);
            }
            write_char(e, ']');
            return;
        }
        default:
            return;
    }
}

void ctsc_emit_js(const CtscNode* sourceFile, const CtscUtf16Buf* source, CtscBuffer* out) {
    Emitter e = { .out = out, .source = source, .indent = 0, .line_start = true };
    emit(&e, sourceFile);
}
