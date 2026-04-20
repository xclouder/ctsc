#include "ctsc/parser.h"
#include "ctsc/arena.h"
#include "ctsc/scanner.h"

#include <string.h>

/*
 * Parser 向 tsc parser.ts 靠拢的渐进实现。目前覆盖：
 *   - SourceFile / EmptyStatement / ExpressionStatement
 *   - VariableStatement (var / let / const)
 *   - FunctionDeclaration (name + (params) + Block body)
 *   - ReturnStatement
 *   - Block
 *   - 表达式优先级（子集）：
 *       primary  : Identifier | NumericLiteral | StringLiteral | ( expr )
 *       call     : primary ( "(" argList ")" )*
 *       additive : call ( "+" | "-" ) call
 *       equality : additive ( "==" | "!=" | "===" | "!==" ) additive
 *       relational : equality ( "<" | ">" | "<=" | ">=" ) equality
 *     （未覆盖：赋值、条件、位/逻辑、new、成员访问等——agent 后续补齐。）
 *
 * 所有 Node.pos 使用 tsc 语义：== token.full_start（含前导 trivia 的起点）。
 */

typedef struct {
    CtscScanner          scanner;
    CtscArena*           arena;
    CtscDiagnosticList*  diagnostics;
    int                  source_end;
    /* Incremented while parsing a Block body of an async function or async arrow
     * (mirrors parser.ts inAwaitContext / parseAwaitExpression ~5713). */
    int                  await_context_depth;
} Parser;

static void advance(Parser* p) { ctsc_scanner_next(&p->scanner); }
static CtscSyntaxKind cur(const Parser* p) { return p->scanner.current.kind; }
static int cur_full_start(const Parser* p) { return p->scanner.current.full_start; }
static int cur_start(const Parser* p)      { return p->scanner.current.start; }
static int cur_end(const Parser* p)        { return p->scanner.current.end; }

static bool accept(Parser* p, CtscSyntaxKind k) {
    if (cur(p) == k) { advance(p); return true; }
    return false;
}

static bool expect(Parser* p, CtscSyntaxKind k) {
    if (accept(p, k)) return true;
    ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005, cur_start(p), cur_end(p) - cur_start(p),
        "'%s' expected.", ctsc_syntax_kind_name(k));
    return false;
}

static CtscNode* make_identifier_from_current(Parser* p) {
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_Identifier, cur_full_start(p), cur_end(p));
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts createIdentifier (~2648):
     * `const text = internIdentifier(scanner.getTokenValue())`. tsc's
     * getTokenValue returns the DECODED identifier name, so for a source like
     * `C\u0032` the Identifier's escapedText is `"C2"` not `"C\\u0032"`.
     * The scanner populates `value` on the slow path (scan_identifier
     * ~scanner.c:620 when a `\uXXXX` escape participates in the name); on
     * the fast path `value` is NULL and `text` already holds the raw name
     * (no escapes to resolve). */
    if (p->scanner.current.value) {
        n->data.identifier.text = p->scanner.current.value;
        n->data.identifier.text_len = p->scanner.current.value_len;
    } else {
        n->data.identifier.text = p->scanner.current.text;
        n->data.identifier.text_len = p->scanner.current.text_len;
    }
    advance(p);
    return n;
}

static CtscNode* parse_identifier(Parser* p) {
    if (cur(p) != CTSC_SK_Identifier) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1003, cur_start(p), cur_end(p) - cur_start(p),
            "Identifier expected.");
        return NULL;
    }
    return make_identifier_from_current(p);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseBindingIdentifier
 * (~2684) + isBindingIdentifier (~2308): accept a real Identifier, OR any
 * token whose kind is past LastReservedWord (ctsc: past CTSC_SK_WithKeyword)
 * — i.e. a contextual / TS-specific keyword used as a binding name. Example:
 * `for (var of; ;) { }` uses the contextual keyword `of` as a variable
 * declarator name (fixtures/emitter/from-upstream/107_parserForOfStatement17.ts).
 * tsc's createIdentifier (~2648) synthesises an Identifier node regardless
 * of the source token kind, so we materialise the node with kind Identifier
 * and the scanner's raw token text.
 */
static CtscNode* parse_binding_identifier(Parser* p) {
    CtscSyntaxKind k = cur(p);
    /* parser.ts parseIdentifier (~2648): `this` as a parameter name uses
     * ThisKeyword in the scanner but materialises as Identifier — needed for
     * `function (this: unknown, ...) {}` (emitter selfhost-derived
     * 74_export_generic_rest.ts). */
    if (k == CTSC_SK_Identifier
        || k == CTSC_SK_ThisKeyword
        || (k >= CTSC_SK_AsKeyword && k <= CTSC_SK_UnknownKeyword)) {
        return make_identifier_from_current(p);
    }
    ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1003, cur_start(p), cur_end(p) - cur_start(p),
        "Identifier expected.");
    return NULL;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts tokenIsIdentifierOrKeyword
 * (scanner.ts ~2898): true for SyntaxKind.Identifier and every reserved /
 * contextual / TS-specific keyword. In ctsc's SyntaxKind enum all keyword kinds
 * are contiguous from CTSC_SK_BreakKeyword through CTSC_SK_UnknownKeyword.
 */
static bool token_is_identifier_or_keyword(CtscSyntaxKind k) {
    if (k == CTSC_SK_Identifier) return true;
    if (k >= CTSC_SK_BreakKeyword && k <= CTSC_SK_UnknownKeyword) return true;
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseRightSideOfDot (~3613)
 * with allowIdentifierNames=true. Any identifier-or-keyword is accepted on the
 * right side of a property-access dot and materialised as a CTSC_SK_Identifier
 * node carrying the token's source text — matching tsc's parseIdentifierName
 * which re-tags a keyword token as an Identifier when it appears in a property
 * name position (e.g. `module.module`, where the right-hand `module` is the
 * ModuleKeyword contextual keyword). We do not yet model hasPrecedingLineBreak
 * ASI recovery or PrivateIdentifier access; callers that need those will grow
 * the helper.
 */
static CtscNode* parse_right_side_of_dot(Parser* p) {
    if (!token_is_identifier_or_keyword(cur(p))) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1003, cur_start(p), cur_end(p) - cur_start(p),
            "Identifier expected.");
        return NULL;
    }
    return make_identifier_from_current(p);
}

static CtscNode* parse_expression(Parser* p);
static CtscNode* parse_assignment_expression(Parser* p);
static CtscNode* parse_argument_expression(Parser* p);
static CtscNode* parse_identifier_or_pattern(Parser* p);
static bool token_is_identifier_expression(CtscSyntaxKind k);
static bool is_let_declaration(Parser* p);
static CtscNode* parse_new_expression(Parser* p);
static CtscNode* make_missing_identifier(Parser* p);
static bool is_yield_expression(Parser* p);
static CtscNode* parse_yield_expression(Parser* p);
static CtscNode* parse_object_literal_expression(Parser* p);
static CtscNode* parse_block(Parser* p);
static CtscNode* parse_parameter(Parser* p);
static bool parse_type_parameters(Parser* p, CtscNodeArray* out);
static CtscNode* parse_type_annotation(Parser* p);
static CtscNode* parse_type_in_annotation_position(Parser* p, bool allow_multiline);
static void skip_type_in_type_parameter_position(Parser* p);
static CtscNode* consume_type_via_fallback_scan(Parser* p, bool allow_multiline);
static void consume_postfix_type_operators(Parser* p);
static CtscNode* parse_class_declaration_or_expression(Parser* p, CtscSyntaxKind kind);
static CtscNode* parse_class_declaration_or_expression_with_modifiers(
    Parser* p, CtscSyntaxKind kind, int pos, const CtscNodeArray* modifiers);
static CtscNode* parse_function_expression(Parser* p);
static bool is_binding_identifier_kind(CtscSyntaxKind k);
static bool is_start_of_statement_token(const Parser* p);
static void parse_modifiers(Parser* p, CtscNodeArray* out);

static CtscNode* parse_template_expression_with_head(Parser* p, int fs, CtscNode* head);
static CtscNode* parse_tagged_template_rest(Parser* p, CtscNode* tag);

static CtscNode* parse_primary(Parser* p) {
    CtscSyntaxKind k = cur(p);
    int fs = cur_full_start(p);
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parsePrimaryExpression
     * (~6619): this / super / null / true / false are emitted as token-only
     * leaf nodes (parseTokenNode<PrimaryExpression>()), preserving their
     * keyword SyntaxKind. The oracle (harness/src/oracle-ast.ts) serializes
     * these via the default branch, which outputs only {kind,pos,end} (no
     * escapedText), because ts.forEachChild on a token leaf visits nothing. */
    if (k == CTSC_SK_ThisKeyword || k == CTSC_SK_SuperKeyword
        || k == CTSC_SK_NullKeyword || k == CTSC_SK_TrueKeyword
        || k == CTSC_SK_FalseKeyword) {
        CtscNode* n = ctsc_node_new(p->arena, k, fs, cur_end(p));
        advance(p);
        return n;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parsePrimaryExpression
     * (~6647, case SyntaxKind.NewKeyword) -> parseNewExpressionOrNewDotTarget
     * (~6801). ctsc does not yet model MetaProperty (`new.target`) or type
     * arguments; the latter is rolled back by tsc when the `<...` fails to
     * close (e.g. the 105_parserConstructorAmbiguity2.ts fixture), so a
     * minimal NewExpression of `new <MemberExpression> (Arguments?)` suffices
     * for current parity. */
    if (k == CTSC_SK_NewKeyword) {
        return parse_new_expression(p);
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parsePrimaryExpression
     * (~6642, case SyntaxKind.ClassKeyword) → parseClassExpression (~8146) →
     * parseClassDeclarationOrExpression(..., SyntaxKind.ClassExpression).
     * The resulting ClassExpression shares its parser and struct layout with
     * ClassDeclaration (the upstream factory call at parser.ts ~8175 is the
     * only place they diverge), so we just forward the expression kind. The
     * fixture that unlocks this branch (107_classExpression1.ts) produces
     * `initializer: {kind:"ClassExpression", children:[Identifier "C"]}`
     * inside a `var v = class C {};` variable declaration. */
    if (k == CTSC_SK_ClassKeyword) {
        return parse_class_declaration_or_expression(p, CTSC_SK_ClassExpression);
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parsePrimaryExpression
     * (~6644, case SyntaxKind.FunctionKeyword) → parseFunctionExpression
     * (~6765). FunctionExpression shares its parser body with
     * FunctionDeclaration (parseFunctionDeclaration / parseFunctionExpression
     * both build on parseSignatureAndBody); the only visible difference is
     * that the name is optional via parseOptionalBindingIdentifier (~6797).
     * Without this dispatch `++function(e) { }` (fixture
     * 107_parserUnaryExpression2.ts) parses as a bare `++` with a missing-
     * identifier operand followed by a top-level FunctionDeclaration, which
     * the JS printer emits as `++;\nfunction (e) { }` instead of the
     * expected `++function (e) { };`. */
    if (k == CTSC_SK_FunctionKeyword) {
        return parse_function_expression(p);
    }
    if (token_is_identifier_expression(k)) {
        /* Contextual keywords (e.g. LetKeyword, AsKeyword, ...) and real
         * identifiers become Identifier in expression position; see
         * parser.ts createIdentifier / isIdentifier. */
        return make_identifier_from_current(p);
    }
    if (k == CTSC_SK_NumericLiteral) {
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_NumericLiteral, fs, cur_end(p));
        /* parseLiteralLikeNode calls factoryCreateNumericLiteral(scanner.getTokenValue()),
         * so the AST's `text` is the normalized token value (e.g. "1" for the
         * LegacyOctal source `01`). See upstream parser.ts ~3768. The scanner
         * sets `value` only when the normalized form differs from source text. */
        if (p->scanner.current.value && p->scanner.current.value_len) {
            n->data.numericLiteral.text = p->scanner.current.value;
            n->data.numericLiteral.text_len = p->scanner.current.value_len;
        } else {
            n->data.numericLiteral.text = p->scanner.current.text;
            n->data.numericLiteral.text_len = p->scanner.current.text_len;
        }
        /* Record the on-disk lexeme so the emitter can mirror tsc's
         * canUseOriginalText branch (utilities.ts ~2036). Suppress it for
         * IsInvalid literals (LegacyOctal / ContainsLeadingZero — see
         * scanner.c scan_number) so the emitter falls back to the canonical
         * tokenValue for those, matching tsc. Also suppress when the lexeme
         * contains numeric separators `_` (TokenFlags.ContainsSeparator — tsc
         * does not reuse original text for those; see utilities.ts
         * canUseOriginalText ~2036). */
        bool has_numeric_sep = false;
        for (size_t ti = 0; ti < p->scanner.current.text_len; ti++) {
            if (p->scanner.current.text[ti] == '_') { has_numeric_sep = true; break; }
        }
        if (p->scanner.current.numeric_literal_is_invalid || has_numeric_sep) {
            n->data.numericLiteral.source_text = NULL;
            n->data.numericLiteral.source_text_len = 0;
        } else {
            n->data.numericLiteral.source_text = p->scanner.current.text;
            n->data.numericLiteral.source_text_len = p->scanner.current.text_len;
        }
        advance(p);
        return n;
    }
    if (k == CTSC_SK_BigIntLiteral) {
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_BigIntLiteral, fs, cur_end(p));
        n->data.numericLiteral.text = p->scanner.current.text;
        n->data.numericLiteral.text_len = p->scanner.current.text_len;
        bool has_numeric_sep = false;
        for (size_t ti = 0; ti < p->scanner.current.text_len; ti++) {
            if (p->scanner.current.text[ti] == '_') { has_numeric_sep = true; break; }
        }
        if (has_numeric_sep) {
            n->data.numericLiteral.source_text = NULL;
            n->data.numericLiteral.source_text_len = 0;
        } else {
            n->data.numericLiteral.source_text = p->scanner.current.text;
            n->data.numericLiteral.source_text_len = p->scanner.current.text_len;
        }
        advance(p);
        return n;
    }
    if (k == CTSC_SK_StringLiteral) {
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_StringLiteral, fs, cur_end(p));
        n->data.stringLiteral.text = p->scanner.current.text;
        n->data.stringLiteral.text_len = p->scanner.current.text_len;
        n->data.stringLiteral.value = p->scanner.current.value;
        n->data.stringLiteral.value_len = p->scanner.current.value_len;
        n->data.stringLiteral.single_quote = (p->scanner.current.text_len > 0 && p->scanner.current.text[0] == '\'');
        advance(p);
        return n;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parsePrimaryExpression
     * (~6648, case SyntaxKind.NoSubstitutionTemplateLiteral) -> parseLiteralNode.
     * Template literals with substitutions (TemplateHead...) go through
     * parseTemplateExpression instead; ctsc will grow that when fixtures demand
     * it. For now a bare `...` becomes a literal-like leaf node. */
    if (k == CTSC_SK_NoSubstitutionTemplateLiteral) {
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_NoSubstitutionTemplateLiteral, fs, cur_end(p));
        /* Record the on-disk lexeme (`...`) so the emitter can mirror tsc's
         * canUseOriginalText branch (utilities.ts ~2036 / getLiteralText ~1980).
         * The scanner stores `text` as a pointer into the source UTF-16 buffer
         * covering the full token including both backticks. */
        n->data.templateLiteralLike.text = p->scanner.current.text;
        n->data.templateLiteralLike.text_len = p->scanner.current.text_len;
        n->data.templateLiteralLike.value = p->scanner.current.value;
        n->data.templateLiteralLike.value_len = p->scanner.current.value_len;
        advance(p);
        return n;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parsePrimaryExpression
     * (~6655, case SyntaxKind.TemplateHead) -> parseTemplateExpression
     * (~3668). Builds `TemplateExpression(head, spans)` where each
     * TemplateSpan pairs `Expression` with its trailing
     * TemplateMiddle / TemplateTail. For this fixture
     * (107_TemplateExpression1.ts `var v = `foo ${ a `) the source ends
     * before any `}`, so parseLiteralOfTemplateSpan (~3713) takes the
     * `else` branch — parseExpectedToken(TemplateTail, ...) synthesises a
     * zero-width missing TemplateTail via createMissingNode (~2619). */
    if (k == CTSC_SK_TemplateHead) {
        CtscNode* head = ctsc_node_new(p->arena, CTSC_SK_TemplateHead, fs, cur_end(p));
        head->data.templateLiteralLike.text = p->scanner.current.text;
        head->data.templateLiteralLike.text_len = p->scanner.current.text_len;
        head->data.templateLiteralLike.value = p->scanner.current.value;
        head->data.templateLiteralLike.value_len = p->scanner.current.value_len;
        advance(p);
        return parse_template_expression_with_head(p, fs, head);
    }
    if (k == CTSC_SK_OpenParenToken) {
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseParenthesizedExpression
         * (~6663): "(" Expression ")". When the expression cannot be parsed
         * (e.g. `()` — see the 106_parser566700.ts fixture `var v = ()({});`),
         * tsc's parseExpression still returns a node because its final fall-
         * through (parsePrimaryExpression ~6660) calls
         * parseIdentifier(Diagnostics.Expression_expected) which synthesizes a
         * zero-width missing Identifier at scanner.getTokenFullStart() (== the
         * `)` full_start). Mirror that here so ParenthesizedExpression.expression
         * is always present, matching tsc byte-for-byte. */
        advance(p);
        CtscNode* e = parse_expression(p);
        if (!e) {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            e = make_missing_identifier(p);
        }
        expect(p, CTSC_SK_CloseParenToken);
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts finishNode (~2600):
         * end = scanner.getTokenFullStart(), i.e. the full_start of the token
         * *after* the consumed `)`. When parseExpected fails (e.g. the fixture
         * `a = (() => { } || a)` where `||` appears inside the outer parens
         * before the inner `)`), parseExpected does NOT advance, so
         * scanner.getTokenFullStart() is the full_start of the unexpected
         * token (the `||` at full_start=33), which matches tsc's
         * ParenthesizedExpression.end=33 for that fixture
         * (fixtures/parser/from-upstream/107_parserArrowFunctionExpression3.ts).
         * Using cur_end(p) *before* expect() instead would incorrectly extend
         * the ParenthesizedExpression through that unexpected token. */
        int close_end = cur_full_start(p);
        CtscNode* paren = ctsc_node_new(p->arena, CTSC_SK_ParenthesizedExpression, fs, close_end);
        paren->data.parenthesizedExpression.expression = e;
        return paren;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseArrayLiteralExpression
     * (~6689) + parseArgumentOrArrayLiteralElement (~6679):
     *     parseDelimitedList(ArrayLiteralMembers, parseArgumentOrArrayLiteralElement)
     * where each element slot calls parseArgumentOrArrayLiteralElement which
     * returns a zero-width OmittedExpression when the current token is `,`
     * (finishNode with no scanner progress: pos == end == getNodePos() ==
     * full_start of the current token). The comma is then consumed by the
     * outer delimited-list separator step. This produces the expected shape
     * for `[1,,1]`: NumericLiteral, OmittedExpression, NumericLiteral.
     *
     * Trailing comma (`[1,]`) is handled by stopping when the next element
     * slot sees `]` (not modelled as OmittedExpression; matches tsc's
     * parseDelimitedList isListTerminator check at ~3474).
     *
     * SpreadElement (`...expr`) is not yet modelled; it will be grown when
     * fixtures demand it. finishNode's end = scanner.getTokenFullStart() of
     * the token AFTER "]" (== cur_full_start after consuming the
     * CloseBracketToken). */
    if (k == CTSC_SK_OpenBracketToken) {
        advance(p); /* "[" */
        CtscNodeArray elements; ctsc_node_array_init(&elements);
        bool has_trailing_comma = false;
        if (cur(p) != CTSC_SK_CloseBracketToken) {
            for (;;) {
                CtscNode* e;
                if (cur(p) == CTSC_SK_CommaToken) {
                    int epos = cur_full_start(p);
                    e = ctsc_node_new(p->arena, CTSC_SK_OmittedExpression, epos, epos);
                } else {
                    e = parse_argument_expression(p);
                    if (!e) break;
                }
                ctsc_node_array_push(&elements, p->arena, e);
                if (!accept(p, CTSC_SK_CommaToken)) break;
                /* Trailing comma: the next slot is `]` — exit without emitting
                 * a phantom OmittedExpression, and flag the element NodeArray
                 * with hasTrailingComma so the emitter can round-trip the
                 * dangling `,`. Mirrors parseDelimitedList's isListTerminator
                 * check (parser.ts ~3474) and the `commaStart >= 0` branch
                 * (~3496) that sets NodeArray.hasTrailingComma. */
                if (cur(p) == CTSC_SK_CloseBracketToken) { has_trailing_comma = true; break; }
            }
        }
        expect(p, CTSC_SK_CloseBracketToken);
        int end = cur_full_start(p);
        CtscNode* arr = ctsc_node_new(p->arena, CTSC_SK_ArrayLiteralExpression, fs, end);
        arr->data.arrayLiteralExpression.elements = elements;
        arr->data.arrayLiteralExpression.has_trailing_comma = has_trailing_comma;
        return arr;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectLiteralExpression
     * (~6755): "{" ObjectLiteralElement (, ObjectLiteralElement)* "}".
     * Defer the element parsing to parse_object_literal_expression so parse_primary
     * stays a dispatch table. The oracle (harness/src/oracle-ast.ts) has no
     * explicit case for ObjectLiteralExpression and falls through to the default
     * branch which serializes forEachChild's visits (the properties list) as a
     * single `children` array (only when non-empty). */
    if (k == CTSC_SK_OpenBraceToken) {
        return parse_object_literal_expression(p);
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parsePrimaryExpression
     * (~6648): SlashToken / SlashEqualsToken at expression start is reinterpreted
     * as a RegularExpressionLiteral via scanner.reScanSlashToken. */
    if (k == CTSC_SK_SlashToken || k == CTSC_SK_SlashEqualsToken) {
        if (ctsc_scanner_re_scan_slash_token(&p->scanner) == CTSC_SK_RegularExpressionLiteral) {
            CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_RegularExpressionLiteral, fs, cur_end(p));
            n->data.regularExpressionLiteral.text = p->scanner.current.text;
            n->data.regularExpressionLiteral.text_len = p->scanner.current.text_len;
            advance(p);
            return n;
        }
    }
    return NULL;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseElementAccessExpressionRest
 * (~6432). On entry the `[` has already been consumed; we parse the argument
 * expression (or synthesise a zero-width missing Identifier when the very
 * next token is `]`, matching tsc's createMissingNode with diagnostic
 * "An_element_access_expression_should_take_an_argument"), consume the `]`,
 * and finish the node with end = scanner.getTokenFullStart() (== cur_full_start
 * after the closing bracket). The resulting node's pos is the caller-supplied
 * `left->pos`, which is the `pos` tsc threads through the rest loop.
 */
static CtscNode* parse_element_access_rest_after_open(Parser* p, CtscNode* left, bool optional_chain) {
    CtscNode* argument;
    if (cur(p) == CTSC_SK_CloseBracketToken) {
        /* Mirrors parser.ts ~6435: createMissingNode(SyntaxKind.Identifier, true,
         * Diagnostics.An_element_access_expression_should_take_an_argument).
         * createMissingNode (~2619) produces a zero-width node at the current
         * token full_start with an empty escapedText. */
        int fs = cur_full_start(p);
        argument = ctsc_node_new(p->arena, CTSC_SK_Identifier, fs, fs);
        argument->data.identifier.text = NULL;
        argument->data.identifier.text_len = 0;
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1011,
            cur_start(p), cur_end(p) - cur_start(p),
            "An element access expression should take an argument.");
    } else {
        argument = parse_expression(p);
        if (!argument) {
            int fs = cur_full_start(p);
            argument = ctsc_node_new(p->arena, CTSC_SK_Identifier, fs, fs);
            argument->data.identifier.text = NULL;
            argument->data.identifier.text_len = 0;
        }
    }
    expect(p, CTSC_SK_CloseBracketToken);
    int end = cur_full_start(p);
    CtscNode* ea = ctsc_node_new(p->arena, CTSC_SK_ElementAccessExpression, left->pos, end);
    ea->data.elementAccessExpression.expression = left;
    ea->data.elementAccessExpression.argumentExpression = argument;
    ea->data.elementAccessExpression.optional_chain = optional_chain;
    return ea;
}

/*
 * Minimal TypeNode parser used from parse_type_arguments_in_expression. It
 * only needs to scan past a single type element so the outer delimited-list
 * loop can see the next `,` or `>`. We parse:
 *   - a plain Identifier type-name → a proper CTSC_SK_TypeReference carrying
 *     typeName=Identifier (plus recursively nested TypeArguments when a `<`
 *     follows). This shape is what the NewExpression oracle relies on (see
 *     `106_parserConstructorAmbiguity3.ts`: tsc emits
 *     `NewExpression.typeArguments=[TypeReference(A)]`).
 *   - as a fallback, a single token we don't model yet (keyword types like
 *     `number`, `string`, literal types, `(...)` tuples, etc.) is absorbed by
 *     brace/bracket/paren/angle depth tracking until we hit a top-level
 *     terminator. The returned node is still a CTSC_SK_TypeReference but
 *     carries no typeName — callers that do not serialize typeArguments (e.g.
 *     CallExpression's try-parse) do not care about its contents.
 *
 * Returns NULL when the speculative parse should give up (e.g. current token
 * cannot start a type). Callers roll back the scanner in that case.
 */
static CtscNode* parse_type_node(Parser* p);

static bool token_can_start_type(CtscSyntaxKind k) {
    /* Subset sufficient for current type-arguments fixtures. Mirrors upstream
     * parser.ts isStartOfType (~4680) for identifier / keyword starts. */
    if (k == CTSC_SK_Identifier) return true;
    if (k == CTSC_SK_AnyKeyword || k == CTSC_SK_UnknownKeyword
        || k == CTSC_SK_NumberKeyword || k == CTSC_SK_StringKeyword
        || k == CTSC_SK_BooleanKeyword || k == CTSC_SK_ObjectKeyword
        || k == CTSC_SK_SymbolKeyword || k == CTSC_SK_VoidKeyword
        || k == CTSC_SK_UndefinedKeyword
        || k == CTSC_SK_ThisKeyword || k == CTSC_SK_NullKeyword
        || k == CTSC_SK_TrueKeyword || k == CTSC_SK_FalseKeyword) return true;
    if (k == CTSC_SK_NumericLiteral || k == CTSC_SK_StringLiteral
        || k == CTSC_SK_BigIntLiteral
        || k == CTSC_SK_NoSubstitutionTemplateLiteral) return true;
    if (k == CTSC_SK_OpenBraceToken || k == CTSC_SK_OpenBracketToken
        || k == CTSC_SK_OpenParenToken) return true;
    /* Contextual/TS keywords that double as type name identifiers. */
    if (k >= CTSC_SK_AsKeyword && k <= CTSC_SK_UnknownKeyword) return true;
    return false;
}

/*
 * Parses a recursive TypeArguments list: `<` Type (, Type)* `>`. Populates
 * `*out_args` with the parsed TypeNodes when parsing succeeds, and returns
 * true. On failure returns false without advancing (caller must roll back
 * the scanner). The enclosing `<`/`>` are consumed when the function succeeds.
 *
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeArgumentsOfTypeReference
 * (~3791): delimited list of parseType terminated by reScanGreaterToken().
 * ctsc's scanner emits `>>` / `>>>` / `>=` as single coalesced tokens; the
 * outer parser must reScanGreaterToken, which we approximate for the current
 * fixture set by only accepting a plain GreaterThanToken terminator.
 */
static bool try_parse_type_argument_list(Parser* p, CtscNodeArray* out_args);

static CtscNode* parse_type_node(Parser* p) {
    CtscSyntaxKind k = cur(p);
    if (!token_can_start_type(k)) return NULL;
    int fs = cur_full_start(p);

    /* Identifier-led type: build a proper CTSC_SK_TypeReference with
     * `typeName` set. Mirrors parser.ts parseTypeReference (~4577):
     * parseEntityNameOfTypeReference → parseTypeArgumentsOfTypeReference?.
     * ctsc currently supports a single-identifier typeName only (no dotted
     * QualifiedName) — sufficient for the fixtures that serialize
     * NewExpression.typeArguments. */
    if (k == CTSC_SK_Identifier) {
        CtscNode* typeName = make_identifier_from_current(p);
        CtscNodeArray type_args; ctsc_node_array_init(&type_args);
        bool has_args = false;
        if (cur(p) == CTSC_SK_LessThanToken) {
            CtscScanner saved = p->scanner;
            size_t saved_diag_count = p->diagnostics->count;
            if (try_parse_type_argument_list(p, &type_args)) {
                has_args = true;
            } else {
                p->scanner = saved;
                ctsc_diag_truncate(p->diagnostics, saved_diag_count);
                ctsc_node_array_init(&type_args);
            }
        }
        int end = cur_full_start(p);
        CtscNode* tr = ctsc_node_new(p->arena, CTSC_SK_TypeReference, fs, end);
        tr->data.typeReference.typeName = typeName;
        tr->data.typeReference.has_type_arguments = has_args;
        tr->data.typeReference.type_arguments = type_args;
        return tr;
    }

    /* Fallback for non-identifier type atoms: consume a single "type atom" —
     * keyword type, literal type, or a brace/bracket/paren group tracked by
     * depth. We do NOT validate the body; the enclosing tryParse will fail
     * the whole attempt if the closing `>` is missing. The returned
     * TypeReference carries no typeName, so callers that serialise
     * TypeReference.children will emit {kind,pos,end} only — which is
     * acceptable for current fixtures (no fixture serialises a keyword-type
     * argument yet). */
    int brace_depth = 0, bracket_depth = 0, paren_depth = 0, angle_depth = 0;
    while (cur(p) != CTSC_SK_EndOfFileToken) {
        CtscSyntaxKind t = cur(p);
        if (brace_depth == 0 && bracket_depth == 0 && paren_depth == 0 && angle_depth == 0) {
            /* Terminators at the top level of a single type element: `,` ends
             * this element in a delimited list; `>` closes the enclosing
             * TypeArguments; `)` / `]` / `}` / `=` / `;` likewise cannot be
             * part of a type here (they terminate the speculative attempt). */
            if (t == CTSC_SK_CommaToken || t == CTSC_SK_GreaterThanToken
                || t == CTSC_SK_CloseParenToken || t == CTSC_SK_CloseBracketToken
                || t == CTSC_SK_CloseBraceToken
                || t == CTSC_SK_SemicolonToken || t == CTSC_SK_EqualsToken) {
                break;
            }
            /* `>>` / `>>>` re-scan back to single `>` by dropping the extra
             * characters — but reScanGreaterToken is applied in the outer
             * loop (see parse_type_arguments_in_expression). Here we just
             * treat any coalesced `>`-family token as a terminator. */
            if (t == CTSC_SK_GreaterThanGreaterThanToken
                || t == CTSC_SK_GreaterThanGreaterThanGreaterThanToken
                || t == CTSC_SK_GreaterThanEqualsToken
                || t == CTSC_SK_GreaterThanGreaterThanEqualsToken
                || t == CTSC_SK_GreaterThanGreaterThanGreaterThanEqualsToken) {
                break;
            }
        }
        if      (t == CTSC_SK_OpenBraceToken)    brace_depth++;
        else if (t == CTSC_SK_CloseBraceToken)   { if (brace_depth == 0) break; brace_depth--; }
        else if (t == CTSC_SK_OpenBracketToken)  bracket_depth++;
        else if (t == CTSC_SK_CloseBracketToken) { if (bracket_depth == 0) break; bracket_depth--; }
        else if (t == CTSC_SK_OpenParenToken)    paren_depth++;
        else if (t == CTSC_SK_CloseParenToken)   { if (paren_depth == 0) break; paren_depth--; }
        else if (t == CTSC_SK_LessThanToken)     angle_depth++;
        else if (t == CTSC_SK_GreaterThanToken && angle_depth > 0) angle_depth--;
        advance(p);
    }
    int end = cur_full_start(p);
    CtscNode* tr = ctsc_node_new(p->arena, CTSC_SK_TypeReference, fs, end);
    tr->data.typeReference.typeName = NULL;
    tr->data.typeReference.has_type_arguments = false;
    ctsc_node_array_init(&tr->data.typeReference.type_arguments);
    return tr;
}

static bool try_parse_type_argument_list(Parser* p, CtscNodeArray* out_args) {
    if (cur(p) != CTSC_SK_LessThanToken) return false;
    advance(p); /* `<` */
    /* Empty `<>` is a syntax error. */
    if (cur(p) == CTSC_SK_GreaterThanToken) return false;
    CtscNodeArray args; ctsc_node_array_init(&args);
    for (;;) {
        CtscNode* t = parse_type_node(p);
        if (!t) return false;
        ctsc_node_array_push(&args, p->arena, t);
        if (cur(p) == CTSC_SK_GreaterThanToken) break;
        if (cur(p) != CTSC_SK_CommaToken) return false;
        advance(p); /* `,` */
    }
    if (cur(p) != CTSC_SK_GreaterThanToken) return false;
    advance(p); /* `>` */
    *out_args = args;
    return true;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeArgumentsInExpression
 * (~6562): speculative parse of `<` TypeList `>`. Returns true iff a valid
 * type-argument list was consumed AND the next token may legitimately follow
 * type arguments in an expression (parser.ts canFollowTypeArgumentsInExpression
 * ~6587). On any failure the caller must restore the saved scanner state.
 *
 * Unlike upstream we do not build a TypeArguments NodeArray — the oracle
 * (harness/src/oracle-ast.ts CallExpression branch) does not visit
 * callExpression.typeArguments, so positional parity for CallExpression's
 * pos/end/expression/arguments is sufficient. This keeps the diff minimal
 * until a fixture demands type-arguments serialization.
 */
static bool try_parse_type_arguments_in_expression_capturing(Parser* p, CtscNodeArray* out_args) {
    /* Try-parse the delimited `<...>` list, then apply the
     * canFollowTypeArgumentsInExpression filter below.
     * Mirrors parser.ts parseTypeArgumentsInExpression ~6562. */
    if (!try_parse_type_argument_list(p, out_args)) return false;
    /* canFollowTypeArgumentsInExpression: the only followers we need to
     * honour for current parity are those that make the type-arguments
     * interpretation unambiguous. Mirrors parser.ts ~6587. */
    CtscSyntaxKind follow = cur(p);
    if (follow == CTSC_SK_OpenParenToken
        || follow == CTSC_SK_NoSubstitutionTemplateLiteral
        || follow == CTSC_SK_TemplateHead) return true;
    if (follow == CTSC_SK_LessThanToken || follow == CTSC_SK_GreaterThanToken
        || follow == CTSC_SK_PlusToken || follow == CTSC_SK_MinusToken) return false;
    /* Favor type-argument interpretation on line break / binary operator /
     * non-expression-starter. For a minimal ctsc parity we accept the
     * remaining tokens that cannot start an expression. */
    if (p->scanner.current.has_preceding_line_break) return true;
    /* Fall back: accept tokens that clearly terminate an expression. */
    switch (follow) {
        case CTSC_SK_CommaToken:
        case CTSC_SK_SemicolonToken:
        case CTSC_SK_CloseParenToken:
        case CTSC_SK_CloseBracketToken:
        case CTSC_SK_CloseBraceToken:
        case CTSC_SK_ColonToken:
        case CTSC_SK_QuestionToken:
        case CTSC_SK_EqualsEqualsToken:
        case CTSC_SK_ExclamationEqualsToken:
        case CTSC_SK_EqualsEqualsEqualsToken:
        case CTSC_SK_ExclamationEqualsEqualsToken:
        case CTSC_SK_AmpersandAmpersandToken:
        case CTSC_SK_BarBarToken:
        case CTSC_SK_QuestionQuestionToken:
        case CTSC_SK_EndOfFileToken:
            return true;
        default:
            return false;
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * isStartOfOptionalPropertyOrElementAccessChain (~6388) with
 * nextTokenIsIdentifierOrKeywordOrOpenBracketOrTemplate (~6381).
 */
static bool is_start_of_optional_property_or_element_access_chain(Parser* p) {
    if (cur(p) != CTSC_SK_QuestionDotToken) return false;
    CtscScanner saved = p->scanner;
    advance(p);
    CtscSyntaxKind k = cur(p);
    p->scanner = saved;
    if (k == CTSC_SK_OpenBracketToken) return true;
    if (k == CTSC_SK_NoSubstitutionTemplateLiteral || k == CTSC_SK_TemplateHead) return true;
    if (k == CTSC_SK_Identifier) return true;
    if (k >= CTSC_SK_BreakKeyword && k <= CTSC_SK_UnknownKeyword) return true;
    return false;
}

/*
 * Continuation of parseTemplateExpression (~3668) after TemplateHead is
 * consumed. Mirrors parseTemplateSpans / parseTemplateSpan with the same
 * reScanTemplateToken behaviour as parseTemplateExpression with isTaggedTemplate false.
 */
static CtscNode* parse_template_expression_with_head(Parser* p, int fs, CtscNode* head) {
    CtscNodeArray spans;
    ctsc_node_array_init(&spans);
    for (;;) {
        int span_fs = cur_full_start(p);
        CtscNode* expr = parse_expression(p);
        if (!expr) {
            expr = make_missing_identifier(p);
        }
        CtscNode* literal;
        int lit_fs = cur_full_start(p);
        if (cur(p) == CTSC_SK_CloseBraceToken) {
            ctsc_scanner_re_scan_template_token(&p->scanner);
            CtscSyntaxKind lk = cur(p);
            if (lk == CTSC_SK_TemplateMiddle || lk == CTSC_SK_TemplateTail) {
                literal = ctsc_node_new(p->arena, lk, lit_fs, cur_end(p));
                literal->data.templateLiteralLike.text = p->scanner.current.text;
                literal->data.templateLiteralLike.text_len = p->scanner.current.text_len;
                literal->data.templateLiteralLike.value = p->scanner.current.value;
                literal->data.templateLiteralLike.value_len = p->scanner.current.value_len;
                advance(p);
            } else {
                literal = ctsc_node_new(p->arena, CTSC_SK_TemplateTail, lit_fs, lit_fs);
                literal->data.templateLiteralLike.text = NULL;
                literal->data.templateLiteralLike.text_len = 0;
                literal->data.templateLiteralLike.value = NULL;
                literal->data.templateLiteralLike.value_len = 0;
            }
        } else if (cur(p) == CTSC_SK_TemplateMiddle || cur(p) == CTSC_SK_TemplateTail) {
            literal = ctsc_node_new(p->arena, cur(p), lit_fs, cur_end(p));
            literal->data.templateLiteralLike.text = p->scanner.current.text;
            literal->data.templateLiteralLike.text_len = p->scanner.current.text_len;
            literal->data.templateLiteralLike.value = p->scanner.current.value;
            literal->data.templateLiteralLike.value_len = p->scanner.current.value_len;
            advance(p);
        } else {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
                cur_start(p), cur_end(p) - cur_start(p),
                "'}' expected.");
            literal = ctsc_node_new(p->arena, CTSC_SK_TemplateTail, lit_fs, lit_fs);
            literal->data.templateLiteralLike.text = NULL;
            literal->data.templateLiteralLike.text_len = 0;
            literal->data.templateLiteralLike.value = NULL;
            literal->data.templateLiteralLike.value_len = 0;
        }
        int span_end = cur_full_start(p);
        CtscNode* span = ctsc_node_new(p->arena, CTSC_SK_TemplateSpan, span_fs, span_end);
        span->data.templateSpan.expression = expr;
        span->data.templateSpan.literal    = literal;
        ctsc_node_array_push(&spans, p->arena, span);
        if (literal->kind != CTSC_SK_TemplateMiddle) break;
    }
    int te_end = cur_full_start(p);
    CtscNode* te = ctsc_node_new(p->arena, CTSC_SK_TemplateExpression, fs, te_end);
    te->data.templateExpression.head = head;
    te->data.templateExpression.templateSpans = spans;
    return te;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTaggedTemplateRest (~6505).
 */
static CtscNode* parse_tagged_template_rest(Parser* p, CtscNode* tag) {
    int pos = tag->pos;
    CtscNode* tmpl;
    if (cur(p) == CTSC_SK_NoSubstitutionTemplateLiteral) {
        tmpl = ctsc_node_new(p->arena, CTSC_SK_NoSubstitutionTemplateLiteral, cur_start(p), cur_end(p));
        tmpl->data.templateLiteralLike.text = p->scanner.current.text;
        tmpl->data.templateLiteralLike.text_len = p->scanner.current.text_len;
        tmpl->data.templateLiteralLike.value = p->scanner.current.value;
        tmpl->data.templateLiteralLike.value_len = p->scanner.current.value_len;
        advance(p);
    } else if (cur(p) == CTSC_SK_TemplateHead) {
        int hfs = cur_full_start(p);
        CtscNode* head = ctsc_node_new(p->arena, CTSC_SK_TemplateHead, hfs, cur_end(p));
        head->data.templateLiteralLike.text = p->scanner.current.text;
        head->data.templateLiteralLike.text_len = p->scanner.current.text_len;
        head->data.templateLiteralLike.value = p->scanner.current.value;
        head->data.templateLiteralLike.value_len = p->scanner.current.value_len;
        advance(p);
        tmpl = parse_template_expression_with_head(p, hfs, head);
    } else {
        return tag;
    }
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_TaggedTemplateExpression, pos, tmpl->end);
    n->data.taggedTemplateExpression.tag = tag;
    n->data.taggedTemplateExpression.template_ = tmpl;
    return n;
}

static CtscNode* parse_call_or_property_rest(Parser* p, CtscNode* left) {
    for (;;) {
        if (cur(p) == CTSC_SK_OpenParenToken) {
            int fs = left->pos;
            advance(p);
            CtscNodeArray args; ctsc_node_array_init(&args);
            if (cur(p) != CTSC_SK_CloseParenToken) {
                /* Mirrors upstream/TypeScript/src/compiler/parser.ts
                 * parseArgumentList (~6556): parseDelimitedList(
                 * ArgumentExpressions, parseArgumentExpression). Each slot is
                 * parseAssignmentExpressionOrHigher (NOT parseExpression) —
                 * a top-level `,` here is the argument separator, not the
                 * comma operator. See parseArgumentExpression (~6685). */
                for (;;) {
                    CtscNode* a = parse_argument_expression(p);
                    if (!a) break;
                    ctsc_node_array_push(&args, p->arena, a);
                    if (!accept(p, CTSC_SK_CommaToken)) break;
                }
            }
            int end = cur_end(p);
            expect(p, CTSC_SK_CloseParenToken);
            CtscNode* call = ctsc_node_new(p->arena, CTSC_SK_CallExpression, fs, end);
            call->data.callExpression.expression = left;
            call->data.callExpression.arguments = args;
            left = call;
            continue;
        }
        /*
         * Optional chaining `?.` before property or element access.
         * Mirrors parser.ts parseMemberExpressionRest (~6457-6473).
         */
        if (cur(p) == CTSC_SK_QuestionDotToken && is_start_of_optional_property_or_element_access_chain(p)) {
            advance(p); /* `?.` */
            if (cur(p) == CTSC_SK_OpenBracketToken) {
                advance(p);
                left = parse_element_access_rest_after_open(p, left, true);
                continue;
            }
            CtscNode* name = parse_right_side_of_dot(p);
            if (!name) break;
            CtscNode* pa = ctsc_node_new(p->arena, CTSC_SK_PropertyAccessExpression, left->pos, name->end);
            pa->data.propertyAccessExpression.expression = left;
            pa->data.propertyAccessExpression.name = name;
            pa->data.propertyAccessExpression.optional_chain = true;
            left = pa;
            continue;
        }
        if (cur(p) == CTSC_SK_DotToken) {
            advance(p);
            /* Mirrors parser.ts parsePropertyAccessExpressionRest (~6415):
             * the RHS of `.` goes through parseRightSideOfDot with
             * allowIdentifierNames=true, which accepts any
             * identifier-or-keyword token and re-tags it as an Identifier. */
            CtscNode* name = parse_right_side_of_dot(p);
            if (!name) break;
            CtscNode* pa = ctsc_node_new(p->arena, CTSC_SK_PropertyAccessExpression, left->pos, name->end);
            pa->data.propertyAccessExpression.expression = left;
            pa->data.propertyAccessExpression.name = name;
            left = pa;
            continue;
        }
        if (cur(p) == CTSC_SK_OpenBracketToken) {
            /* Mirrors parser.ts parseMemberExpressionRest (~6471):
             *     parseOptional(SyntaxKind.OpenBracketToken) ->
             *     parseElementAccessExpressionRest. */
            advance(p);
            left = parse_element_access_rest_after_open(p, left, false);
            continue;
        }
        /* Mirrors parser.ts parseMemberExpressionRest (~6476):
         * isTemplateStartOfTaggedTemplate -> parseTaggedTemplateRest. */
        if (cur(p) == CTSC_SK_NoSubstitutionTemplateLiteral || cur(p) == CTSC_SK_TemplateHead) {
            left = parse_tagged_template_rest(p, left);
            continue;
        }
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseCallExpressionRest
         * (~6520, esp. ~6526 `tryParse(parseTypeArgumentsInExpression)`): when
         * a `<` appears after a LeftHandSide, speculatively try to parse it as
         * a type-argument list; if it closes with `>` and is followed by `(`
         * (or a template literal) we absorb it as a CallExpression. Otherwise
         * rollback the scanner so the outer binary-expression parser can
         * interpret the `<` as a relational operator (e.g. `a < b`). */
        if (cur(p) == CTSC_SK_LessThanToken) {
            CtscScanner saved = p->scanner;
            size_t saved_diag_count = p->diagnostics->count;
            CtscNodeArray discard; ctsc_node_array_init(&discard);
            bool ok = try_parse_type_arguments_in_expression_capturing(p, &discard);
            if (ok && cur(p) == CTSC_SK_OpenParenToken) {
                /* Success: consume the argument list as a CallExpression. */
                int fs = left->pos;
                advance(p); /* `(` */
                CtscNodeArray args; ctsc_node_array_init(&args);
                if (cur(p) != CTSC_SK_CloseParenToken) {
                    for (;;) {
                        CtscNode* a = parse_argument_expression(p);
                        if (!a) break;
                        ctsc_node_array_push(&args, p->arena, a);
                        if (!accept(p, CTSC_SK_CommaToken)) break;
                    }
                }
                int end = cur_end(p);
                expect(p, CTSC_SK_CloseParenToken);
                CtscNode* call = ctsc_node_new(p->arena, CTSC_SK_CallExpression, fs, end);
                call->data.callExpression.expression = left;
                call->data.callExpression.arguments = args;
                left = call;
                continue;
            }
            /* Rollback: restore scanner and drop diagnostics emitted during
             * the speculative parse (mirrors tryParse / speculationHelper). */
            p->scanner = saved;
            ctsc_diag_truncate(p->diagnostics, saved_diag_count);
            break;
        }
        break;
    }
    return left;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseMemberExpressionRest
 * (~6453) restricted to the non-tagged-template subset currently modelled in
 * ctsc: property access (`.name`), element access (`[argument]`), and
 * type-arguments absorption (`<...>`). Unlike parse_call_or_property_rest,
 * this helper intentionally stops at the first `(` so that `new X(args)`
 * builds a NewExpression (arguments consumed by parse_new_expression below)
 * rather than CallExpression(X, args). Optional chains (`?.`) for property /
 * element access are handled the same way as in parse_call_or_property_rest.
 *
 * Type arguments are handled exactly as in parser.ts ~6490 — a speculative
 * `tryParse(parseTypeArgumentsInExpression)` attempt. In upstream, a
 * successful parse wraps the expression as ExpressionWithTypeArguments;
 * parseNewExpressionOrNewDotTarget (~6812) then unpacks those typeArguments
 * onto the surrounding NewExpression. ctsc does not yet materialise the
 * intermediate ExpressionWithTypeArguments node; instead, when the caller
 * is parse_new_expression it passes `out_type_args` / `out_has_type_args`
 * so the absorbed list is surfaced upward for attachment to the
 * NewExpression (mirroring what tsc's unpack step does). Callers that
 * don't care (e.g. post-primary chaining on a LeftHandSide) pass NULL and
 * the absorbed tokens are discarded — which stays byte-for-byte compatible
 * because the oracle has no explicit case for ExpressionWithTypeArguments
 * either.
 *
 * Subsequent `.name` or `[...]` chaining rewraps `left` as a new
 * MemberExpression; per tsc, that discards any previously absorbed type
 * arguments (the ExpressionWithTypeArguments wrapper becomes the inner
 * expression of the new MemberExpression, and the outer property access
 * owns no typeArguments of its own). We mirror that by clearing
 * `*out_has_type_args` in those branches.
 */
static CtscNode* parse_member_expression_rest(Parser* p, CtscNode* left,
                                              CtscNodeArray* out_type_args,
                                              bool* out_has_type_args) {
    for (;;) {
        if (cur(p) == CTSC_SK_QuestionDotToken && is_start_of_optional_property_or_element_access_chain(p)) {
            advance(p); /* `?.` */
            if (cur(p) == CTSC_SK_OpenBracketToken) {
                advance(p);
                left = parse_element_access_rest_after_open(p, left, true);
                if (out_has_type_args) *out_has_type_args = false;
                continue;
            }
            CtscNode* name = parse_right_side_of_dot(p);
            if (!name) break;
            CtscNode* pa = ctsc_node_new(p->arena, CTSC_SK_PropertyAccessExpression, left->pos, name->end);
            pa->data.propertyAccessExpression.expression = left;
            pa->data.propertyAccessExpression.name = name;
            pa->data.propertyAccessExpression.optional_chain = true;
            left = pa;
            if (out_has_type_args) *out_has_type_args = false;
            continue;
        }
        if (cur(p) == CTSC_SK_DotToken) {
            advance(p);
            /* Mirrors parser.ts parsePropertyAccessExpressionRest (~6415):
             * allowIdentifierNames=true so a keyword token (e.g. the
             * ModuleKeyword in `module.module`) is accepted as the
             * property name. */
            CtscNode* name = parse_right_side_of_dot(p);
            if (!name) break;
            CtscNode* pa = ctsc_node_new(p->arena, CTSC_SK_PropertyAccessExpression, left->pos, name->end);
            pa->data.propertyAccessExpression.expression = left;
            pa->data.propertyAccessExpression.name = name;
            left = pa;
            if (out_has_type_args) *out_has_type_args = false;
            continue;
        }
        if (cur(p) == CTSC_SK_OpenBracketToken) {
            advance(p);
            left = parse_element_access_rest_after_open(p, left, false);
            if (out_has_type_args) *out_has_type_args = false;
            continue;
        }
        if (cur(p) == CTSC_SK_LessThanToken) {
            CtscScanner saved = p->scanner;
            size_t saved_diag_count = p->diagnostics->count;
            CtscNodeArray parsed; ctsc_node_array_init(&parsed);
            if (try_parse_type_arguments_in_expression_capturing(p, &parsed)) {
                if (out_type_args && out_has_type_args) {
                    *out_type_args = parsed;
                    *out_has_type_args = true;
                }
                continue;
            }
            p->scanner = saved;
            ctsc_diag_truncate(p->diagnostics, saved_diag_count);
            break;
        }
        break;
    }
    return left;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseNewExpressionOrNewDotTarget
 * (~6801):
 *     NewExpression:
 *       `new` MemberExpression TypeArguments? Arguments?
 * tsc's parseMemberExpressionRest (~6490) greedily wraps `expr<T>` as an
 * ExpressionWithTypeArguments, and parseNewExpressionOrNewDotTarget (~6812)
 * then unpacks those typeArguments onto the NewExpression. ctsc takes the
 * same net effect but without materializing the intermediate
 * ExpressionWithTypeArguments: after parseMemberExpressionRest stops, we
 * speculatively try to parse `<...>` as a type-argument list — on success
 * they become NewExpression.typeArguments (with `canFollowTypeArgumentsInExpression`
 * as the arbitrator so `new Date<A` without the closing `>` rolls back the
 * scanner and the outer binary parser sees `<` as a relational operator,
 * matching the 105_parserConstructorAmbiguity2.ts fixture).
 *
 * finishNode end (~2600): scanner.getTokenFullStart() of the next token.
 *   - Without anything trailing:  cur_full_start after parseMemberExpressionRest.
 *   - With typeArguments but no `(`:  cur_full_start after consuming `>`.
 *   - With arguments: cur_full_start AFTER consuming `)`, == tsc's CallArguments
 *     finishNode end.
 */
static CtscNode* parse_new_expression(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* `new` */
    CtscNode* expression = parse_primary(p);
    bool has_type_args = false;
    CtscNodeArray type_args; ctsc_node_array_init(&type_args);
    if (!expression) {
        /* Mirrors parser.ts parsePrimaryExpression (~6660) default branch when
         * no primary is available: synthesize a zero-width missing Identifier. */
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        expression = make_missing_identifier(p);
    } else {
        expression = parse_member_expression_rest(p, expression, &type_args, &has_type_args);
    }
    /* After parse_member_expression_rest, if typeArguments were absorbed by
     * the final `<...>` in the chain, they logically sit on the last
     * MemberExpression (tsc's ExpressionWithTypeArguments wrapper). Lifting
     * them onto the NewExpression mirrors parser.ts parseNewExpressionOrNewDotTarget
     * (~6812) which unpacks the wrapper. `expression->end` already includes
     * the `>` because parse_member_expression_rest advanced the scanner past
     * it; but the node itself wasn't re-finished, so we need to re-align the
     * end. scanner.getTokenFullStart() at this point is the right value. */
    int expr_end = has_type_args ? cur_full_start(p) : expression->end;

    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_NewExpression, fs, expr_end);
    n->data.newExpression.expression = expression;
    n->data.newExpression.has_type_arguments = has_type_args;
    n->data.newExpression.type_arguments = type_args;
    n->data.newExpression.has_arguments = false;
    ctsc_node_array_init(&n->data.newExpression.arguments);
    if (cur(p) == CTSC_SK_OpenParenToken) {
        advance(p);
        if (cur(p) != CTSC_SK_CloseParenToken) {
            for (;;) {
                CtscNode* a = parse_argument_expression(p);
                if (!a) break;
                ctsc_node_array_push(&n->data.newExpression.arguments, p->arena, a);
                if (!accept(p, CTSC_SK_CommaToken)) break;
            }
        }
        int end = cur_end(p);
        expect(p, CTSC_SK_CloseParenToken);
        n->end = end;
        n->data.newExpression.has_arguments = true;
    }
    return n;
}

/* Simple prefix unary operators that take a UnaryExpression operand.
 * Mirrors parser.ts parseSimpleUnaryExpression (~5796): + - ~ ! (ctsc does not
 * yet model delete/typeof/void/await/TypeAssertion). */
static bool is_simple_prefix_unary_op(CtscSyntaxKind k) {
    return k == CTSC_SK_PlusToken || k == CTSC_SK_MinusToken
        || k == CTSC_SK_ExclamationToken || k == CTSC_SK_TildeToken;
}

/*
 * Mirrors parser.ts createMissingNode<Identifier>(SyntaxKind.Identifier, ...)
 * (~2619): a zero-width Identifier whose pos == end == scanner.getTokenFullStart()
 * and whose escapedText is the empty string. Used when parseLeftHandSideExpressionOrHigher
 * has no primary to parse (e.g. `++;`).
 */
static CtscNode* make_missing_identifier(Parser* p) {
    int fs = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_Identifier, fs, fs);
    n->data.identifier.text = NULL;
    n->data.identifier.text_len = 0;
    return n;
}

/*
 * Mirrors parser.ts parseLeftHandSideExpressionOrHigher (~5897):
 * primary (+ member/call chain). Returns NULL when there is no primary; callers
 * decide whether to emit a missing Identifier (tsc's parsePrimaryExpression
 * default branch uses parseIdentifier(Expression_expected) to do the same).
 */
static CtscNode* parse_left_hand_side_expression_or_higher(Parser* p) {
    CtscNode* n = parse_primary(p);
    if (!n) return NULL;
    return parse_call_or_property_rest(p, n);
}

/*
 * Mirrors parser.ts parseUpdateExpression (~5875):
 *   ++/-- LHS              => PrefixUnaryExpression
 *   LHS [no LT here] ++/-- => PostfixUnaryExpression
 *   LHS                    => LHS
 * PrefixUnaryExpression.end is operand.end (finishNode uses pos + scanner
 * state after parsing the operand). PostfixUnaryExpression.end is
 * scanner.getTokenFullStart() AFTER the ++/-- token is consumed, i.e. the
 * full_start of the token that follows (== what `finishNode` would record).
 */
static CtscNode* parse_update_expression(Parser* p) {
    CtscSyntaxKind k = cur(p);
    if (k == CTSC_SK_PlusPlusToken || k == CTSC_SK_MinusMinusToken) {
        int fs = cur_full_start(p);
        advance(p);
        CtscNode* operand = parse_left_hand_side_expression_or_higher(p);
        if (!operand) {
            /* Mirrors parser.ts parsePrimaryExpression (~6660) default branch:
             * when the current token cannot start an expression, synthesize a
             * zero-width missing Identifier at the current full_start. */
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109, cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            operand = make_missing_identifier(p);
        }
        CtscNode* u = ctsc_node_new(p->arena, CTSC_SK_PrefixUnaryExpression, fs, operand->end);
        u->data.prefixUnaryExpression.operator_kind = k;
        u->data.prefixUnaryExpression.operand = operand;
        return u;
    }
    CtscNode* lhs = parse_left_hand_side_expression_or_higher(p);
    if (!lhs) return NULL;
    if ((cur(p) == CTSC_SK_PlusPlusToken || cur(p) == CTSC_SK_MinusMinusToken)
        && !p->scanner.current.has_preceding_line_break) {
        CtscSyntaxKind op = cur(p);
        advance(p);
        /* finishNode end: scanner.getTokenFullStart() of the token AFTER ++/--
         * (== full_start of the current token, since we just consumed ++/--). */
        int end = cur_full_start(p);
        CtscNode* u = ctsc_node_new(p->arena, CTSC_SK_PostfixUnaryExpression, lhs->pos, end);
        u->data.postfixUnaryExpression.operator_kind = op;
        u->data.postfixUnaryExpression.operand = lhs;
        return u;
    }
    return lhs;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseNonArrayType (~4589)
 * for the keyword-type arm: AnyKeyword / UnknownKeyword / StringKeyword /
 * NumberKeyword / BigIntKeyword / SymbolKeyword / BooleanKeyword /
 * UndefinedKeyword / NeverKeyword / ObjectKeyword all dispatch to
 *     tryParse(parseKeywordAndNoDot) || parseTypeReference();
 * parseKeywordAndNoDot (~4523) consumes the keyword as a single-token TypeNode
 * when the next token is NOT `.`, returning a node whose kind *is* the
 * keyword SyntaxKind (not TypeReference). That's what tsc emits for the
 * `type` child of a TypeAssertionExpression — e.g. `<any>0` yields
 * `{kind:"AnyKeyword",pos,end}` as the first forEachChild visit. Returns
 * NULL when the current token is not one of these keyword types, so the
 * caller can fall back to the generic type parser.
 */
static CtscNode* try_parse_keyword_type_no_dot(Parser* p) {
    CtscSyntaxKind tk = cur(p);
    if (tk != CTSC_SK_AnyKeyword && tk != CTSC_SK_UnknownKeyword
        && tk != CTSC_SK_StringKeyword && tk != CTSC_SK_NumberKeyword
        && tk != CTSC_SK_SymbolKeyword && tk != CTSC_SK_BooleanKeyword
        && tk != CTSC_SK_UndefinedKeyword && tk != CTSC_SK_ObjectKeyword
        && tk != CTSC_SK_VoidKeyword && tk != CTSC_SK_NullKeyword) {
        return NULL;
    }
    int fs = cur_full_start(p);
    CtscScanner saved = p->scanner;
    advance(p);
    if (cur(p) == CTSC_SK_DotToken) {
        p->scanner = saved;
        return NULL;
    }
    int end = cur_full_start(p);
    return ctsc_node_new(p->arena, tk, fs, end);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeAssertion (~6371):
 *     parseExpected(LessThanToken);
 *     const type = parseType();
 *     parseExpected(GreaterThanToken);
 *     const expression = parseSimpleUnaryExpression();
 *     return finishNode(factory.createTypeAssertion(type, expression), pos);
 * Dispatched from parseSimpleUnaryExpression (~5809) when the current token
 * is LessThanToken (and languageVariant != JSX). ctsc only supports the
 * keyword-type / identifier-type arms of parseType today — sufficient for
 * the 108_destructuringTypeAssertionsES5_5.ts fixture (`<any>0`) and the
 * wider set of TS assertions we expect to see.
 */
static CtscNode* parse_unary(Parser* p);

static CtscNode* parse_type_assertion(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_LessThanToken);
    CtscNode* type = try_parse_keyword_type_no_dot(p);
    if (!type) type = parse_type_node(p);
    expect(p, CTSC_SK_GreaterThanToken);
    CtscNode* expression = parse_unary(p);
    if (!expression) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        expression = make_missing_identifier(p);
    }
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_TypeAssertionExpression, fs, expression->end);
    n->data.typeAssertionExpression.type = type;
    n->data.typeAssertionExpression.expression = expression;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isAwaitExpression (~5713):
 * in await context always; otherwise lookAhead(nextTokenIsIdentifierOrKeywordOrLiteralOnSameLine).
 */
static bool is_await_expression(Parser* p) {
    if (cur(p) != CTSC_SK_AwaitKeyword) return false;
    if (p->await_context_depth > 0) return true;
    CtscScanner saved = p->scanner;
    advance(p);
    bool same_line = !p->scanner.current.has_preceding_line_break;
    bool ok = false;
    if (same_line) {
        CtscSyntaxKind nk = cur(p);
        ok = is_binding_identifier_kind(nk) || nk == CTSC_SK_NumericLiteral
            || nk == CTSC_SK_StringLiteral || nk == CTSC_SK_NoSubstitutionTemplateLiteral
            || nk == CTSC_SK_RegularExpressionLiteral
            || (nk >= CTSC_SK_BreakKeyword && nk <= CTSC_SK_UnknownKeyword);
    }
    p->scanner = saved;
    return ok;
}

static CtscNode* parse_unary(Parser* p) {
    if (is_simple_prefix_unary_op(cur(p))) {
        int fs = cur_full_start(p);
        CtscSyntaxKind op = cur(p);
        advance(p);
        CtscNode* operand = parse_unary(p);
        if (!operand) {
            /* Mirrors parser.ts parsePrimaryExpression (~6660) default branch:
             * when nextTokenAnd(parseSimpleUnaryExpression) bottoms out at a
             * token that cannot start an expression (e.g. an Unknown null
             * character, as in the 105_scannerUnexpectedNullCharacter1.ts
             * fixture), parseIdentifier(Expression_expected) synthesizes a
             * zero-width missing Identifier at scanner.getTokenFullStart().
             * Returning NULL here would leave the `+` / `-` / `!` / `~` token
             * unconsumed and cause the outer parseList loop to re-enter at
             * the same token forever. */
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            operand = make_missing_identifier(p);
        }
        CtscNode* u = ctsc_node_new(p->arena, CTSC_SK_PrefixUnaryExpression, fs, operand->end);
        u->data.prefixUnaryExpression.operator_kind = op;
        u->data.prefixUnaryExpression.operand = operand;
        return u;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseSimpleUnaryExpression
     * (~5796): the three keyword-prefix unary expressions `delete`, `typeof`,
     * `void` each dispatch to a tiny parser (parseDeleteExpression ~5698,
     * parseTypeOfExpression ~5703, parseVoidExpression ~5708) that consumes
     * the keyword, recursively parses another UnaryExpression as the operand,
     * and wraps it in the matching node kind. ctsc stores all three shapes in
     * the shared voidExpression data struct (see ast.h ~336).
     * finishNode end defaults to scanner.getTokenFullStart() AFTER the operand
     * is parsed, which equals operand->end here (ctsc's finishNode convention
     * for prefix-unary; see PrefixUnaryExpression above). */
    if (cur(p) == CTSC_SK_VoidKeyword
        || cur(p) == CTSC_SK_DeleteKeyword
        || cur(p) == CTSC_SK_TypeOfKeyword) {
        CtscSyntaxKind kw = cur(p);
        int fs = cur_full_start(p);
        advance(p); /* void / delete / typeof */
        CtscNode* operand = parse_unary(p);
        if (!operand) {
            /* Same recovery as simple prefix unary above: parsePrimaryExpression
             * (~6660) bottoms out at parseIdentifier(Expression_expected),
             * synthesizing a zero-width missing Identifier so the resulting
             * expression still has a well-defined operand.end. */
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            operand = make_missing_identifier(p);
        }
        CtscSyntaxKind node_kind =
            kw == CTSC_SK_DeleteKeyword ? CTSC_SK_DeleteExpression :
            kw == CTSC_SK_TypeOfKeyword ? CTSC_SK_TypeOfExpression :
                                          CTSC_SK_VoidExpression;
        CtscNode* v = ctsc_node_new(p->arena, node_kind, fs, operand->end);
        v->data.voidExpression.expression = operand;
        return v;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseSimpleUnaryExpression
     * (~5819): case AwaitKeyword when isAwaitExpression → parseAwaitExpression (~5726). */
    if (cur(p) == CTSC_SK_AwaitKeyword && is_await_expression(p)) {
        int fs = cur_full_start(p);
        advance(p);
        CtscNode* operand = parse_unary(p);
        if (!operand) {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            operand = make_missing_identifier(p);
        }
        CtscNode* aw = ctsc_node_new(p->arena, CTSC_SK_AwaitExpression, fs, operand->end);
        aw->data.awaitExpression.expression = operand;
        return aw;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseSimpleUnaryExpression
     * (~5809): when the current token is `<` in non-JSX mode, dispatch to
     * parseTypeAssertion to parse `< Type > UnaryExpression`. Without this
     * the `<` is left to the binary-expression precedence climber, which
     * would parse `<any>0` as a relational-then-relational BinaryExpression
     * (see fixture 108_destructuringTypeAssertionsES5_5.ts). JSX mode is not
     * yet modelled; when it is, this branch needs the JSX-element dispatch
     * from upstream (~5812). */
    if (cur(p) == CTSC_SK_LessThanToken) {
        return parse_type_assertion(p);
    }
    return parse_update_expression(p);
}

static bool is_multiplicative_op(CtscSyntaxKind k) {
    return k == CTSC_SK_AsteriskToken || k == CTSC_SK_SlashToken || k == CTSC_SK_PercentToken;
}
static bool is_additive_op(CtscSyntaxKind k) {
    return k == CTSC_SK_PlusToken || k == CTSC_SK_MinusToken;
}
static bool is_shift_op(CtscSyntaxKind k) {
    return k == CTSC_SK_LessThanLessThanToken
        || k == CTSC_SK_GreaterThanGreaterThanToken
        || k == CTSC_SK_GreaterThanGreaterThanGreaterThanToken;
}
static bool is_equality_op(CtscSyntaxKind k) {
    return k == CTSC_SK_EqualsEqualsToken || k == CTSC_SK_ExclamationEqualsToken
        || k == CTSC_SK_EqualsEqualsEqualsToken || k == CTSC_SK_ExclamationEqualsEqualsToken;
}
static bool is_relational_op(CtscSyntaxKind k) {
    /* Mirrors upstream getBinaryOperatorPrecedence (~5990): relational includes
     * `instanceof` and `in` (parser.ts parseBinaryExpressionRest ~5608). */
    return k == CTSC_SK_LessThanToken || k == CTSC_SK_GreaterThanToken
        || k == CTSC_SK_LessThanEqualsToken || k == CTSC_SK_GreaterThanEqualsToken
        || k == CTSC_SK_InstanceOfKeyword
        || k == CTSC_SK_InKeyword;
}
static bool is_logical_and_op(CtscSyntaxKind k) { return k == CTSC_SK_AmpersandAmpersandToken; }

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseBinaryExpressionRest (~5608):
 * before testing the next binary operator, re-scan a bare '>' so that '>>', '>>>',
 * and '>=' tokens are coalesced from the generic-friendly single-'>' emission.
 *
 * When the right-hand side cannot be parsed (e.g. `yield *;`), tsc still produces
 * a BinaryExpression because parsePrimaryExpression's default branch (~6660) calls
 * parseIdentifier(Expression_expected), which synthesizes a zero-width missing
 * Identifier at scanner.getTokenFullStart(). ctsc mirrors that here instead of
 * making parse_primary always return a node, keeping other callers' NULL-means-
 * "no primary" contract intact.
 *
 * Symmetrically, when the LEFT-hand side cannot be parsed but the current token
 * IS a binary operator at this level, tsc's parseBinaryExpressionOrHigher (~5551)
 * still builds a BinaryExpression because parseUnaryExpressionOrHigher bottoms
 * out at parsePrimaryExpression's default branch (~6660) and returns a zero-
 * width missing Identifier. The 107_parseIncompleteBinaryExpression1.ts fixture
 * (`var v = || b;`) exercises this: the initializer starts with `||`, so the
 * inner parseUnary call returns a missing Identifier, parseBinaryExpressionRest
 * then consumes `||` and the right operand `b`, yielding a BinaryExpression with
 * a zero-width left. Mirror that here by synthesizing the missing Identifier at
 * exactly the level that owns the operator (is_op(cur) == true).
 */
static CtscNode* parse_binary_level(Parser* p, CtscNode* (*down)(Parser*), bool (*is_op)(CtscSyntaxKind)) {
    CtscNode* left = down(p);
    if (!left) {
        /* Re-scan a bare `>` so that `>=` / `>>` / `>>>` are coalesced before
         * the is_op test below (parser.ts parseBinaryExpressionRest ~5608). */
        if (cur(p) == CTSC_SK_GreaterThanToken) {
            ctsc_scanner_re_scan_greater_token(&p->scanner);
        }
        if (!is_op(cur(p))) return NULL;
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        left = make_missing_identifier(p);
    }
    for (;;) {
        if (cur(p) == CTSC_SK_GreaterThanToken) {
            ctsc_scanner_re_scan_greater_token(&p->scanner);
        }
        if (!is_op(cur(p))) break;
        CtscSyntaxKind op = cur(p);
        advance(p);
        CtscNode* right = down(p);
        if (!right) {
            /* Mirrors parser.ts parsePrimaryExpression (~6660) default branch:
             * parseIdentifier(Diagnostics.Expression_expected) -> createMissingNode
             * (~2619) with pos==end==scanner.getTokenFullStart(). */
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            right = make_missing_identifier(p);
        }
        CtscNode* bin = ctsc_node_new(p->arena, CTSC_SK_BinaryExpression, left->pos, right->end);
        bin->data.binaryExpression.left = left;
        bin->data.binaryExpression.operator_kind = op;
        bin->data.binaryExpression.right = right;
        left = bin;
    }
    return left;
}

static CtscNode* parse_multiplicative(Parser* p) { return parse_binary_level(p, parse_unary,          is_multiplicative_op); }
static CtscNode* parse_additive(Parser* p)       { return parse_binary_level(p, parse_multiplicative, is_additive_op); }
static CtscNode* parse_shift(Parser* p)          { return parse_binary_level(p, parse_additive,       is_shift_op); }
/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseBinaryExpressionRest (~5608):
 * relational operators plus same-line `as Type` (parser.ts ~5649-5662).
 */
static CtscNode* parse_relational(Parser* p) {
    CtscNode* left = parse_shift(p);
    if (!left) {
        if (cur(p) == CTSC_SK_GreaterThanToken) {
            ctsc_scanner_re_scan_greater_token(&p->scanner);
        }
        if (!is_relational_op(cur(p))) return NULL;
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        left = make_missing_identifier(p);
    }
    for (;;) {
        if (cur(p) == CTSC_SK_GreaterThanToken) {
            ctsc_scanner_re_scan_greater_token(&p->scanner);
        }
        if (cur(p) == CTSC_SK_AsKeyword && !p->scanner.current.has_preceding_line_break) {
            advance(p);
            CtscNode* type = parse_type_in_annotation_position(p, true);
            CtscNode* asx = ctsc_node_new(p->arena, CTSC_SK_AsExpression, left->pos, type->end);
            asx->data.asExpression.expression = left;
            asx->data.asExpression.type = type;
            left = asx;
            continue;
        }
        if (!is_relational_op(cur(p))) break;
        CtscSyntaxKind op = cur(p);
        advance(p);
        CtscNode* right = parse_shift(p);
        if (!right) {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            right = make_missing_identifier(p);
        }
        CtscNode* bin = ctsc_node_new(p->arena, CTSC_SK_BinaryExpression, left->pos, right->end);
        bin->data.binaryExpression.left = left;
        bin->data.binaryExpression.operator_kind = op;
        bin->data.binaryExpression.right = right;
        left = bin;
    }
    return left;
}
static CtscNode* parse_equality(Parser* p)       { return parse_binary_level(p, parse_relational,     is_equality_op); }
static CtscNode* parse_logical_and(Parser* p)    { return parse_binary_level(p, parse_equality,       is_logical_and_op); }
/*
 * Mirrors upstream/TypeScript/src/compiler/utilities.ts getBinaryOperatorPrecedence
 * (~5990): `||` and `??` share OperatorPrecedence.Coalesce / LogicalOR (utilities.ts
 * ~5754-5759), so they parse at the same level with left associativity.
 */
static bool is_logical_or_or_nullish_coalesce_op(CtscSyntaxKind k) {
    return k == CTSC_SK_BarBarToken || k == CTSC_SK_QuestionQuestionToken;
}
static CtscNode* parse_logical_or(Parser* p) {
    return parse_binary_level(p, parse_logical_and, is_logical_or_or_nullish_coalesce_op);
}

static CtscNode* parse_conditional(Parser* p) {
    CtscNode* cond = parse_logical_or(p);
    if (!cond) return NULL;
    if (cur(p) != CTSC_SK_QuestionToken) return cond;
    advance(p);
    CtscNode* whenTrue = parse_assignment_expression(p);
    expect(p, CTSC_SK_ColonToken);
    CtscNode* whenFalse = parse_assignment_expression(p);
    CtscNode* c = ctsc_node_new(p->arena, CTSC_SK_ConditionalExpression, cond->pos, whenFalse ? whenFalse->end : cur_end(p));
    c->data.conditionalExpression.condition = cond;
    c->data.conditionalExpression.whenTrue = whenTrue;
    c->data.conditionalExpression.whenFalse = whenFalse;
    return c;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/utilities.ts isAssignmentOperator
 * (~7435): token is in [FirstAssignment .. LastAssignment] range. ctsc keeps
 * a subset of the upstream enum, so we enumerate the supported kinds here
 * instead of using an ordered range check.
 */
static bool is_assignment_op(CtscSyntaxKind k) {
    switch (k) {
        case CTSC_SK_EqualsToken:
        case CTSC_SK_PlusEqualsToken:
        case CTSC_SK_MinusEqualsToken:
        case CTSC_SK_AsteriskEqualsToken:
        case CTSC_SK_SlashEqualsToken:
        case CTSC_SK_PercentEqualsToken:
        case CTSC_SK_GreaterThanGreaterThanEqualsToken:
        case CTSC_SK_GreaterThanGreaterThanGreaterThanEqualsToken:
            return true;
        default:
            return false;
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/utilitiesPublic.ts
 * isLeftHandSideExpressionKind (~1999). Only kinds that end in an LHS
 * production can appear on the left of an assignment; a BinaryExpression
 * (e.g. `1 >> <missing>` in the 105_parserGreaterThanTokenAmbiguity12.ts
 * fixture) must NOT be treated as an assignment target so that the `=`
 * is left for statement-level recovery and ASI splits the source into
 * two statements (parser.ts parseAssignmentExpressionOrHigher ~5128).
 * Kinds ctsc does not yet model (JsxElement, NonNullExpression,
 * ExpressionWithTypeArguments, MetaProperty, MissingDeclaration,
 * PrivateIdentifier, BigIntLiteral) are omitted for now; they will be grown
 * alongside the parser productions that create them.
 */
static bool is_left_hand_side_expression_kind(CtscSyntaxKind k) {
    switch (k) {
        case CTSC_SK_PropertyAccessExpression:
        case CTSC_SK_ElementAccessExpression:
        case CTSC_SK_NewExpression:
        case CTSC_SK_CallExpression:
        case CTSC_SK_TaggedTemplateExpression:
        case CTSC_SK_TemplateExpression:
        case CTSC_SK_ArrayLiteralExpression:
        case CTSC_SK_ParenthesizedExpression:
        case CTSC_SK_ObjectLiteralExpression:
        case CTSC_SK_Identifier:
        case CTSC_SK_RegularExpressionLiteral:
        case CTSC_SK_NumericLiteral:
        case CTSC_SK_StringLiteral:
        case CTSC_SK_NoSubstitutionTemplateLiteral:
        case CTSC_SK_FalseKeyword:
        case CTSC_SK_NullKeyword:
        case CTSC_SK_ThisKeyword:
        case CTSC_SK_TrueKeyword:
        case CTSC_SK_SuperKeyword:
            return true;
        default:
            return false;
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isYieldExpression (~5136)
 * restricted to the non-yield-context branch: the parser does not yet track
 * the [Yield] grammar parameter, so we apply the fallback heuristic
 * unconditionally — `yield` followed on the same line by an identifier /
 * keyword / numeric / big-int / string literal starts a YieldExpression
 * (mirrors nextTokenIsIdentifierOrKeywordOrLiteralOnSameLine ~7163). This is
 * sufficient for the 105_YieldExpression2_es6.ts fixture and for `yield expr`
 * inside generators (which also satisfy the same lookahead).
 */
static bool is_yield_expression(Parser* p) {
    if (cur(p) != CTSC_SK_YieldKeyword) return false;
    CtscScanner saved = p->scanner;
    advance(p);
    CtscSyntaxKind next = cur(p);
    bool same_line = !p->scanner.current.has_preceding_line_break;
    p->scanner = saved;
    if (!same_line) return false;
    if (next == CTSC_SK_Identifier) return true;
    if (next == CTSC_SK_NumericLiteral || next == CTSC_SK_BigIntLiteral
        || next == CTSC_SK_StringLiteral) return true;
    /* tokenIsIdentifierOrKeyword: reserved words + contextual/TS keywords. */
    if (next >= CTSC_SK_BreakKeyword && next <= CTSC_SK_UnknownKeyword) return true;
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseYieldExpression (~5169):
 *   yield
 *   yield [no LineTerminator here] AssignmentExpression
 *   yield [no LineTerminator here] * AssignmentExpression
 *
 * finishNode (~2600) end = scanner.getTokenFullStart() of the next token.
 *   - Bare `yield`: after advancing past YieldKeyword the scanner sits on the
 *     next token; its full_start is `cur_full_start(p)` == end we want.
 *   - With expression: after parsing the AssignmentExpression operand, finishNode
 *     records the scanner position AT its own return; this equals the operand's
 *     node.end (which itself is a finishNode-produced full_start of the next
 *     token), so we reuse operand->end.
 */
static CtscNode* parse_yield_expression(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* yield */
    bool same_line = !p->scanner.current.has_preceding_line_break;
    bool is_asterisk = cur(p) == CTSC_SK_AsteriskToken;
    /* Approximate isStartOfExpression: any of the primary-expression starters
     * or unary operator starts we already recognise. If it's not, fall through
     * to the bare `yield` form (matches upstream's `else` branch). */
    bool starts_expr = is_asterisk
        || is_simple_prefix_unary_op(cur(p))
        || cur(p) == CTSC_SK_PlusPlusToken || cur(p) == CTSC_SK_MinusMinusToken
        || cur(p) == CTSC_SK_VoidKeyword
        || cur(p) == CTSC_SK_NewKeyword
        || cur(p) == CTSC_SK_OpenParenToken || cur(p) == CTSC_SK_OpenBracketToken
        || cur(p) == CTSC_SK_OpenBraceToken
        || cur(p) == CTSC_SK_NumericLiteral || cur(p) == CTSC_SK_StringLiteral
        || cur(p) == CTSC_SK_NoSubstitutionTemplateLiteral
        || cur(p) == CTSC_SK_SlashToken || cur(p) == CTSC_SK_SlashEqualsToken
        || token_is_identifier_expression(cur(p));
    CtscNode* y = ctsc_node_new(p->arena, CTSC_SK_YieldExpression, fs, cur_full_start(p));
    y->data.yieldExpression.has_asterisk = false;
    y->data.yieldExpression.asterisk_pos = 0;
    y->data.yieldExpression.asterisk_end = 0;
    y->data.yieldExpression.expression = NULL;
    if (same_line && starts_expr) {
        if (is_asterisk) {
            y->data.yieldExpression.has_asterisk = true;
            y->data.yieldExpression.asterisk_pos = cur_full_start(p);
            y->data.yieldExpression.asterisk_end = cur_end(p);
            advance(p);
        }
        CtscNode* expr = parse_assignment_expression(p);
        if (expr) {
            y->data.yieldExpression.expression = expr;
            y->end = expr->end;
        } else {
            y->end = cur_full_start(p);
        }
    }
    return y;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * isParenthesizedArrowFunctionExpression (~5236) + worker (~5251): decide
 * whether the current token stream (possibly starting at `(` or an identifier)
 * is a parenthesized arrow-function head. We implement a minimal subset
 * sufficient for the currently-unlocked fixture (107_parserArrowFunctionExpression2.ts
 * `a = () => { } || a`):
 *   - `( )` immediately followed by `=>` / `:` / `{`  →  True.
 *   - `( Identifier )` followed by `=>` / `:`          →  True.
 *   - `( Identifier , ...`                              →  True (multi-param).
 *   - `( Identifier =`  (default value)                 →  True.
 * Everything else returns False, so the parser falls back to
 * ParenthesizedExpression. That is a strict subset of the upstream Tristate
 * worker (we never return Unknown / speculate), but it covers every arrow
 * production we need to emit today without risking regressions on the
 * already-green ParenthesizedExpression fixtures.
 *
 * `=>` appearing at the very start (e.g. `=> foo`) is treated as True by
 * upstream (~5241 ERROR RECOVERY TWEAK); we intentionally don't yet emit a
 * recovery path for that — no unlocked fixture exercises it.
 */
static bool looks_like_parenthesized_arrow_function(Parser* p) {
    if (cur(p) != CTSC_SK_OpenParenToken) return false;
    CtscScanner saved = p->scanner;
    size_t saved_diag_count = p->diagnostics->count;
    advance(p); /* `(` */
    CtscSyntaxKind second = cur(p);
    bool result = false;
    if (second == CTSC_SK_CloseParenToken) {
        advance(p); /* `)` */
        CtscSyntaxKind third = cur(p);
        result = (third == CTSC_SK_EqualsGreaterThanToken
               || third == CTSC_SK_ColonToken
               || third == CTSC_SK_OpenBraceToken);
    } else if (second == CTSC_SK_DotDotDotToken) {
        /* parser.ts ~5294: "Simple case: `(...`. This is an arrow function
         * with a rest parameter." Always Tristate.True. Fixture
         * 107_parserParameterList11.ts (`(...arg?) => 102`) drives this. */
        result = true;
    } else if (second == CTSC_SK_Identifier
               || (second >= CTSC_SK_BreakKeyword && second <= CTSC_SK_UnknownKeyword)) {
        advance(p); /* identifier/keyword */
        CtscSyntaxKind third = cur(p);
        /* `(a,` / `(a=` / `(a:` / `(a)` / `(a?` all could start an arrow; the
         * `)`-only case is ambiguous with a ParenthesizedExpression. Commit to
         * ArrowFunction when the disambiguator is unambiguous OR when a `)`
         * is followed by `=>` / `:`, OR when `?` is followed by
         * `:` / `,` / `=` / `)` (parser.ts ~5321-5326: `(a?:` / `(a?,` /
         * `(a?=` / `(a?)` are "definitely a lambda"). */
        if (third == CTSC_SK_CommaToken || third == CTSC_SK_ColonToken
            || third == CTSC_SK_EqualsToken) {
            result = true;
        } else if (third == CTSC_SK_CloseParenToken) {
            advance(p); /* `)` */
            CtscSyntaxKind fourth = cur(p);
            result = (fourth == CTSC_SK_EqualsGreaterThanToken
                   || fourth == CTSC_SK_ColonToken);
        } else if (third == CTSC_SK_QuestionToken) {
            advance(p); /* `?` */
            CtscSyntaxKind fourth = cur(p);
            result = (fourth == CTSC_SK_ColonToken
                   || fourth == CTSC_SK_CommaToken
                   || fourth == CTSC_SK_EqualsToken
                   || fourth == CTSC_SK_CloseParenToken);
        }
    }
    p->scanner = saved;
    ctsc_diag_truncate(p->diagnostics, saved_diag_count);
    return result;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * parseArrowFunctionExpressionBody (~5533): a `{` starts a BlockFunctionBody;
 * anything else is an AssignmentExpression concise body. ctsc does not
 * model the isStartOfStatement recovery branch (~5538) — no unlocked
 * fixture needs it — so we take the simpler two-way split.
 */
static CtscNode* parse_arrow_function_body(Parser* p, bool is_async) {
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        if (is_async) p->await_context_depth++;
        CtscNode* b = parse_block(p);
        if (is_async) p->await_context_depth--;
        return b;
    }
    CtscNode* expr = parse_assignment_expression(p);
    if (!expr) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        expr = make_missing_identifier(p);
    }
    return expr;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * parseParenthesizedArrowFunctionExpression (~5430) + parseSimpleArrowFunctionExpression
 * (~5197): parse the parameter list (wrapped in `(...)` or a single bare
 * Identifier), the `=>` token, and the ConciseBody (Block or AssignmentExpression).
 * Callers must have already verified via looks_like_parenthesized_arrow_function
 * (for the parenthesized form) or the single-identifier + `=>` lookahead in
 * parse_assignment_expression that the current token stream is indeed an
 * arrow-function head; this helper does not speculate.
 *
 * Finish-node positions follow parser.ts finishNode (~2600):
 *     node.pos = fs   (== full_start of the first token of the head)
 *     node.end = body.end
 * The EqualsGreaterThanToken pos/end are captured so ast_json.c can emit
 * it as a synthetic token leaf between `parameters` and `body` in the
 * forEachChild-driven `children` array.
 */
/*
 * Helper: does the current token begin a Parameter? Mirrors parser.ts
 * isStartOfParameter (~3993) for the non-JSDoc, non-modifier subset we
 * currently model: BindingIdentifier (incl. contextual/TS keywords accepted
 * by is_binding_identifier_kind) OR an ObjectBindingPattern `{` / Array
 * BindingPattern `[` (destructuring). DotDotDot / decorators / modifiers are
 * skipped until a fixture demands them.
 */
static bool is_start_of_parameter(const Parser* p) {
    CtscSyntaxKind k = cur(p);
    if (k == CTSC_SK_OpenBraceToken || k == CTSC_SK_OpenBracketToken) return true;
    /* parser.ts isStartOfParameter (~3993): DotDotDotToken is a parameter
     * start (rest parameter). Required so the parenthesized-arrow parameter
     * loop above routes `(...x) => ...` into parse_parameter rather than
     * abortParsingListOrMoveToNextToken. */
    if (k == CTSC_SK_DotDotDotToken) return true;
    /* `this` is a reserved word but valid as the first parameter (this-
     * typing); parseBindingIdentifier materialises it as Identifier. */
    if (k == CTSC_SK_ThisKeyword) return true;
    return is_binding_identifier_kind(k);
}

/*
 * Parses `( ParameterList ) ReturnType? => Body` after the opening `(` has
 * been verified. `arrow_pos` is the ArrowFunction node `pos` (full start of
 * `<` for a generic arrow, or of `(` for a plain parenthesized arrow).
 * `type_parameters` is moved into the ArrowFunction on success (cleared).
 * When `speculative` is true and the token after the signature is neither
 * `=>` nor `{`, returns NULL so callers can roll back (mirrors upstream
 * parseParenthesizedArrowFunctionExpression ~5490 with allowAmbiguity false).
 */
static CtscNode* parse_arrow_from_open_paren(
    Parser* p,
    bool is_async_arrow,
    int arrow_pos,
    CtscNodeArray* type_parameters,
    bool speculative)
{
    int fs = arrow_pos;
    expect(p, CTSC_SK_OpenParenToken);
    CtscNodeArray params; ctsc_node_array_init(&params);
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseDelimitedList
     * (~3489) for ParsingContext.Parameters with error recovery:
     *   - isListElement = isStartOfParameter (~2913 / 3993): Binding
     *     Identifier, BindingPattern `{`/`[`, or a dot-dot-dot / modifier.
     *   - isListTerminator = CloseParen / CloseBracket / EOF (~3034).
     *   - abortParsingListOrMoveToNextToken (~3410) skips a stray token when
     *     no outer context claims it. For arrow parameter recovery the
     *     outer SourceElements context would reclaim e.g. a FunctionKeyword,
     *     but does NOT reclaim `=>`, `:`, `.`, etc. — those are advanced
     *     past so the parser can find the next parameter start.
     *
     * The 107_parserErrorRecovery_ParameterList5.ts fixture
     * (`(a:number => { }`) drives this recovery: after Parameter `a:number`
     * the current token is `=>`, which is not a parameter start, not a list
     * terminator, and not a statement start — so we skip past it, find `{`
     * as a BindingPattern parameter start, parse Parameter 2 as
     * ObjectBindingPattern `{ }`, then the next iteration's list-terminator
     * check (EOF) exits the loop. */
    for (;;) {
        if (cur(p) == CTSC_SK_CloseParenToken) break;
        if (cur(p) == CTSC_SK_EndOfFileToken) break;
        if (is_start_of_parameter(p)) {
            int el_fs = cur_full_start(p);
            CtscNode* param = parse_parameter(p);
            if (!param) break;
            ctsc_node_array_push(&params, p->arena, param);
            if (accept(p, CTSC_SK_CommaToken)) continue;
            if (cur(p) == CTSC_SK_CloseParenToken) break;
            if (cur(p) == CTSC_SK_EndOfFileToken) break;
            /* parseExpected(CommaToken) emits "',' expected." without
             * advancing. The zero-progress guard (parser.ts ~3532) advances
             * the scanner when parse_parameter consumed no tokens —
             * defensive, parse_parameter always makes progress via
             * parse_identifier_or_pattern. */
            if (el_fs == cur_full_start(p)) advance(p);
            continue;
        }
        /* Stray token between / before parameters. Mirrors
         * abortParsingListOrMoveToNextToken (~3410) + isInSomeParsingContext
         * (~3078): if the outer SourceElements context would claim the token
         * (i.e. it starts a statement), break so the arrow function bails
         * out with a zero-parameter list and the outer parser resumes.
         * Otherwise advance past it. `{` / `[` are parameter starts (handled
         * above) so they never reach this branch even though
         * is_start_of_statement_token also accepts `{` as a BlockStatement
         * start. */
        if (is_start_of_statement_token(p) && cur(p) != CTSC_SK_SemicolonToken) break;
        advance(p);
    }
    /* parseExpected(CloseParenToken): emits the diagnostic when absent and
     * does NOT advance. The enclosing ArrowFunction's end is driven by the
     * body / missing-`=>` position, not by the `)`. */
    expect(p, CTSC_SK_CloseParenToken);
    /* parseReturnType(ColonToken, isType=false) (~4093): consumes `:`
     * followed by a Type if present. Return type is not yet serialised in
     * the ArrowFunction oracle shape, so we just advance the scanner. */
    if (cur(p) == CTSC_SK_ColonToken) {
        (void)parse_type_annotation(p);
    }
    CtscSyntaxKind last_token = cur(p);
    if (speculative && last_token != CTSC_SK_EqualsGreaterThanToken
        && last_token != CTSC_SK_OpenBraceToken) {
        return NULL;
    }
    /* parser.ts ~5497:
     *     const lastToken = token();
     *     const equalsGreaterThanToken = parseExpectedToken(EqualsGreaterThanToken);
     *     const body = (lastToken === EqualsGreaterThanToken || lastToken === OpenBraceToken)
     *         ? parseArrowFunctionExpressionBody(...)
     *         : parseIdentifier();
     * `lastToken` captures the current token BEFORE parseExpectedToken runs
     * so that, when `=>` is missing and the next token is neither `=>` nor
     * `{`, the body falls back to parseIdentifier() which materialises a
     * zero-width missing Identifier at scanner.getTokenFullStart() (via
     * createMissingNode ~2619). This is exactly the shape the
     * 107_parserErrorRecovery_ParameterList5.ts fixture expects:
     *     EqualsGreaterThanToken pos=35 end=35, Identifier pos=35 end=35
     *     escapedText="". */
    int eg_pos, eg_end;
    if (cur(p) == CTSC_SK_EqualsGreaterThanToken) {
        eg_pos = cur_full_start(p);
        eg_end = cur_end(p);
        advance(p);
    } else {
        /* parseExpectedToken: emit diagnostic, synthesise a missing token
         * at the current scanner position (pos=end=full_start). Mirrors
         * parser.ts parseExpectedToken → createMissingNode (~2619). */
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'=>' expected.");
        eg_pos = cur_full_start(p);
        eg_end = cur_full_start(p);
    }
    CtscNode* body;
    if (last_token == CTSC_SK_EqualsGreaterThanToken
        || last_token == CTSC_SK_OpenBraceToken) {
        body = parse_arrow_function_body(p, is_async_arrow);
    } else {
        /* parseIdentifier(): when the current token is not an identifier,
         * createIdentifier falls through to createMissingNode(Identifier)
         * which yields a zero-width Identifier at scanner.getTokenFullStart()
         * with escapedText = "". */
        if (cur(p) == CTSC_SK_Identifier || is_binding_identifier_kind(cur(p))) {
            body = make_identifier_from_current(p);
        } else {
            body = make_missing_identifier(p);
        }
    }
    int end = body ? body->end : cur_full_start(p);
    CtscNode* af = ctsc_node_new(p->arena, CTSC_SK_ArrowFunction, fs, end);
    af->data.arrowFunction.has_async = is_async_arrow;
    af->data.arrowFunction.type_parameters = *type_parameters;
    ctsc_node_array_init(type_parameters);
    af->data.arrowFunction.parameters = params;
    af->data.arrowFunction.equals_greater_than_pos = eg_pos;
    af->data.arrowFunction.equals_greater_than_end = eg_end;
    af->data.arrowFunction.body = body;
    return af;
}

static CtscNode* parse_parenthesized_arrow_function(Parser* p, bool is_async_arrow) {
    CtscNodeArray empty_tps;
    ctsc_node_array_init(&empty_tps);
    return parse_arrow_from_open_paren(
        p, is_async_arrow, cur_full_start(p), &empty_tps, false);
}

static CtscNode* parse_async_parenthesized_arrow_function(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* async */
    CtscNode* af = parse_parenthesized_arrow_function(p, true);
    if (af && af->kind == CTSC_SK_ArrowFunction) {
        af->pos = fs;
        af->data.arrowFunction.has_async = true;
    }
    return af;
}

static CtscNode* parse_simple_arrow_function(Parser* p, CtscNode* identifier) {
    /* Caller has already parsed the single bare Identifier and verified that
     * the next token is `=>`. Build the one-element parameters NodeArray
     * (mirrors parser.ts parseSimpleArrowFunctionExpression ~5197). */
    CtscNodeArray params; ctsc_node_array_init(&params);
    CtscNode* param = ctsc_node_new(p->arena, CTSC_SK_Parameter, identifier->pos, identifier->end);
    param->data.parameter.name = identifier;
    ctsc_node_array_push(&params, p->arena, param);
    int eg_pos = cur_full_start(p);
    int eg_end = cur_end(p);
    advance(p); /* `=>` */
    CtscNode* body = parse_arrow_function_body(p, false);
    int end = body ? body->end : cur_full_start(p);
    CtscNode* af = ctsc_node_new(p->arena, CTSC_SK_ArrowFunction, identifier->pos, end);
    af->data.arrowFunction.has_async = false;
    af->data.arrowFunction.parameters = params;
    af->data.arrowFunction.equals_greater_than_pos = eg_pos;
    af->data.arrowFunction.equals_greater_than_end = eg_end;
    af->data.arrowFunction.body = body;
    return af;
}

/*
 * Assignment is right-associative: a = b = c.
 *
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * parseAssignmentExpressionOrHigher (~5069): after parsing the LHS, call
 * reScanGreaterToken so that sequences like `> > =` coalesce into `>>=` (and
 * similarly `>>>=`) before the assignment-operator check (parser.ts ~5128).
 *
 * Assignment is only consumed when the LHS is a LeftHandSideExpression
 * (parser.ts ~5128 `isLeftHandSideExpression(expr) && isAssignmentOperator(...)`).
 * Otherwise (e.g. the `1 >> <missing>` BinaryExpression produced by the
 * 105_parserGreaterThanTokenAmbiguity12.ts fixture), the `=` is left for
 * statement-level recovery so ASI can split the source into two statements.
 *
 * ArrowFunction productions (4 and 5) are short-circuited here before any
 * LHS / binary parsing: parser.ts ~5097 tries
 * tryParseParenthesizedArrowFunctionExpression first, and only on failure
 * drops to parseBinaryExpressionOrHigher. ctsc currently models only the
 * parenthesized form and the single-identifier shorthand (`x => body`);
 * the async variant is not yet supported.
 */
static CtscNode* parse_assignment_expression(Parser* p) {
    /* Mirrors parser.ts parseAssignmentExpressionOrHigher (~5082): YieldExpression
     * must be checked before any LHS / binary productions so `yield foo` is not
     * parsed as an Identifier expression followed by a second statement. */
    if (is_yield_expression(p)) {
        return parse_yield_expression(p);
    }
    /* Mirrors parser.ts tryParseAsyncSimpleArrowFunctionExpression (~5395):
     * async [no LT] ( ... ) => ... */
    if (cur(p) == CTSC_SK_AsyncKeyword && !p->scanner.current.has_preceding_line_break) {
        CtscScanner saved = p->scanner;
        advance(p);
        bool ok = !p->scanner.current.has_preceding_line_break && cur(p) == CTSC_SK_OpenParenToken
            && looks_like_parenthesized_arrow_function(p);
        p->scanner = saved;
        if (ok) {
            return parse_async_parenthesized_arrow_function(p);
        }
    }
    /*
     * Mirrors upstream parser.ts isParenthesizedArrowFunctionExpression (~5236):
     * the worker treats LessThanToken like OpenParenToken for generic arrow
     * heads (`<T>(a) => ...`). parsePossibleParenthesizedArrowFunctionExpression
     * (~5381) speculatively parses; we mirror the rollback with a saved
     * scanner + diagnostic checkpoint (see parseParenthesizedArrowFunctionExpression
     * ~5430 + ~5490 allowAmbiguity false).
     */
    if (cur(p) == CTSC_SK_LessThanToken) {
        CtscScanner saved = p->scanner;
        size_t saved_diag_count = p->diagnostics->count;
        int arrow_fs = cur_full_start(p);
        CtscNodeArray tps;
        ctsc_node_array_init(&tps);
        (void)parse_type_parameters(p, &tps);
        if (cur(p) == CTSC_SK_OpenParenToken && looks_like_parenthesized_arrow_function(p)) {
            CtscNode* af = parse_arrow_from_open_paren(p, false, arrow_fs, &tps, true);
            if (af) return af;
        }
        p->scanner = saved;
        ctsc_diag_truncate(p->diagnostics, saved_diag_count);
    }
    /* Production 4: parenthesized ArrowFunctionExpression. Mirrors parser.ts
     * tryParseParenthesizedArrowFunctionExpression (~5216). We commit only
     * when the lookahead is unambiguous — see looks_like_parenthesized_arrow_function.
     * The upstream Tristate.Unknown branch (speculative parse + rollback)
     * is not yet modelled; adding it will require tracking the scanner /
     * diagnostic checkpoint across parseParametersWorker. */
    if (cur(p) == CTSC_SK_OpenParenToken
        && looks_like_parenthesized_arrow_function(p)) {
        return parse_parenthesized_arrow_function(p, false);
    }
    CtscNode* left = parse_conditional(p);
    if (!left) return NULL;
    /* Production 4 (simple form): `Identifier =>`. Mirrors parser.ts ~5118:
     * after parsing a BinaryExpression, if it is a single Identifier and
     * the current token is `=>`, wrap as ArrowFunction via
     * parseSimpleArrowFunctionExpression (~5197). */
    if (left->kind == CTSC_SK_Identifier
        && cur(p) == CTSC_SK_EqualsGreaterThanToken
        && !p->scanner.current.has_preceding_line_break) {
        return parse_simple_arrow_function(p, left);
    }
    if (cur(p) == CTSC_SK_GreaterThanToken) {
        ctsc_scanner_re_scan_greater_token(&p->scanner);
    }
    if (is_left_hand_side_expression_kind(left->kind) && is_assignment_op(cur(p))) {
        CtscSyntaxKind op = cur(p);
        advance(p);
        CtscNode* right = parse_assignment_expression(p);
        if (!right) return left;
        CtscNode* b = ctsc_node_new(p->arena, CTSC_SK_BinaryExpression, left->pos, right->end);
        b->data.binaryExpression.left = left;
        b->data.binaryExpression.operator_kind = op;
        b->data.binaryExpression.right = right;
        return b;
    }
    return left;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseArgumentExpression
 * (~6685) / parseArgumentOrArrayLiteralElement (~6679): optional DotDotDotToken
 * then AssignmentExpression → SpreadElement; otherwise AssignmentExpression.
 */
static CtscNode* parse_argument_expression(Parser* p) {
    if (cur(p) == CTSC_SK_DotDotDotToken) {
        int fs = cur_full_start(p);
        advance(p);
        CtscNode* expr = parse_assignment_expression(p);
        if (!expr) {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            expr = make_missing_identifier(p);
        }
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_SpreadElement, fs, expr->end);
        n->data.spreadElement.expression = expr;
        return n;
    }
    return parse_assignment_expression(p);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseExpression (~5041):
 *   Expression[in]:
 *       AssignmentExpression[in]
 *       Expression[in] , AssignmentExpression[in]
 *
 * Left-associative: fold each `,`-separated AssignmentExpression into a
 * BinaryExpression(CommaToken). The resulting BinaryExpression's pos is
 * the initial getNodePos() (== the first sub-expression's pos, which
 * includes leading trivia), and its end is the last sub-expression's end
 * — makeBinaryExpression in parser.ts calls finishNode with the same
 * saved pos and the scanner at the end of the right operand.
 *
 * Callers that must NOT allow a top-level comma (CallExpression arguments,
 * ArrayLiteral elements, VariableDeclaration initializer, conditional
 * branches, ternary branches) call parse_assignment_expression directly,
 * matching tsc's parseArgumentExpression / parseInitializer split.
 */
static CtscNode* parse_expression(Parser* p) {
    CtscNode* expr = parse_assignment_expression(p);
    if (!expr) return NULL;
    while (cur(p) == CTSC_SK_CommaToken) {
        advance(p); /* consume `,` */
        CtscNode* right = parse_assignment_expression(p);
        if (!right) {
            /* Mirrors parser.ts parseAssignmentExpressionOrHigher — which
             * always returns a node because parsePrimaryExpression's default
             * branch (~6660) calls parseIdentifier(Diagnostics.Expression_expected),
             * synthesising a zero-width missing Identifier at the current
             * scanner full_start. The 108_parserX_TypeArgumentList1.ts fixture
             * exercises this: `Foo<A,B,\ C>(...)` rolls back type-argument
             * parsing and interprets `<` as a relational operator, leaving the
             * outer comma expression with a trailing `,` before the invalid
             * `\` character. tsc still produces an outer
             * `BinaryExpression(Comma)` whose right is a missing Identifier
             * whose pos/end coincide with the `\` token's full_start (== 27
             * for that fixture), extending the first ExpressionStatement to
             * end=27. */
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            right = make_missing_identifier(p);
        }
        CtscNode* b = ctsc_node_new(p->arena, CTSC_SK_BinaryExpression, expr->pos, right->end);
        b->data.binaryExpression.left = expr;
        b->data.binaryExpression.operator_kind = CTSC_SK_CommaToken;
        b->data.binaryExpression.right = right;
        expr = b;
    }
    return expr;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseComputedPropertyName
 * (~2733): `[ Expression ]`. finishNode sets end = scanner.getTokenFullStart()
 * of the token AFTER `]` (== cur_full_start(p) once we've consumed `]`). When
 * the expression slot is empty (e.g. `[]`) tsc's parseExpression still returns
 * a node via parsePrimaryExpression's final parseIdentifier(Expression_expected)
 * fallback (~6660) which synthesises a zero-width missing Identifier — we
 * mirror that here so ComputedPropertyName.expression is always present.
 */
static CtscNode* parse_computed_property_name(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_OpenBracketToken);
    CtscNode* expr = parse_expression(p);
    if (!expr) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        expr = make_missing_identifier(p);
    }
    expect(p, CTSC_SK_CloseBracketToken);
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_ComputedPropertyName, fs, end);
    n->data.computedPropertyName.expression = expr;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parsePropertyName (~2729)
 * restricted to the identifier/keyword/numeric/string/ComputedPropertyName
 * subset exercised by current fixtures. When the current token cannot start a
 * property name, we synthesize a zero-width missing Identifier at
 * scanner.getTokenFullStart() (see createMissingNode(Identifier) ~2619 /
 * createIdentifier ~2665 fallback path), matching tsc's `var v = { *{ } }`
 * recovery where parsePropertyName falls through to parseIdentifier with an
 * empty escapedText.
 */
static CtscNode* parse_property_name(Parser* p) {
    CtscSyntaxKind k = cur(p);
    int fs = cur_full_start(p);
    if (k == CTSC_SK_OpenBracketToken) {
        return parse_computed_property_name(p);
    }
    if (k == CTSC_SK_PrivateIdentifier) {
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts
         * parsePropertyNameWorker (~2714) dispatching to
         * parsePrivateIdentifier (~2747) when the current token is `#name`.
         * finishNode positions: pos = getNodePos (= scanner.getTokenFullStart
         * when the `#name` token is current), end = scanner.getTokenFullStart
         * AFTER nextToken() — captured here as cur_end before advance; the
         * forEachChild walker treats PrivateIdentifier as a leaf so the
         * oracle's default branch emits `{kind,pos,end}` with no children
         * and no escapedText. ctsc stores the decoded name in
         * identifier.text so future lookups (binder / checker) can reuse it
         * without re-scanning — ast_json.c's PrivateIdentifier case omits
         * the field to match tsc's oracle byte-for-byte. */
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_PrivateIdentifier, fs, cur_end(p));
        if (p->scanner.current.value) {
            n->data.identifier.text = p->scanner.current.value;
            n->data.identifier.text_len = p->scanner.current.value_len;
        } else {
            n->data.identifier.text = p->scanner.current.text;
            n->data.identifier.text_len = p->scanner.current.text_len;
        }
        advance(p);
        return n;
    }
    if (k == CTSC_SK_StringLiteral) {
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_StringLiteral, fs, cur_end(p));
        n->data.stringLiteral.text = p->scanner.current.text;
        n->data.stringLiteral.text_len = p->scanner.current.text_len;
        n->data.stringLiteral.value = p->scanner.current.value;
        n->data.stringLiteral.value_len = p->scanner.current.value_len;
        n->data.stringLiteral.single_quote =
            (p->scanner.current.text_len > 0 && p->scanner.current.text[0] == '\'');
        advance(p);
        return n;
    }
    if (k == CTSC_SK_NumericLiteral) {
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_NumericLiteral, fs, cur_end(p));
        if (p->scanner.current.value && p->scanner.current.value_len) {
            n->data.numericLiteral.text = p->scanner.current.value;
            n->data.numericLiteral.text_len = p->scanner.current.value_len;
        } else {
            n->data.numericLiteral.text = p->scanner.current.text;
            n->data.numericLiteral.text_len = p->scanner.current.text_len;
        }
        if (p->scanner.current.numeric_literal_is_invalid) {
            n->data.numericLiteral.source_text = NULL;
            n->data.numericLiteral.source_text_len = 0;
        } else {
            n->data.numericLiteral.source_text = p->scanner.current.text;
            n->data.numericLiteral.source_text_len = p->scanner.current.text_len;
        }
        advance(p);
        return n;
    }
    if (k == CTSC_SK_BigIntLiteral) {
        CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_BigIntLiteral, fs, cur_end(p));
        n->data.numericLiteral.text = p->scanner.current.text;
        n->data.numericLiteral.text_len = p->scanner.current.text_len;
        bool has_numeric_sep = false;
        for (size_t ti = 0; ti < p->scanner.current.text_len; ti++) {
            if (p->scanner.current.text[ti] == '_') { has_numeric_sep = true; break; }
        }
        if (has_numeric_sep) {
            n->data.numericLiteral.source_text = NULL;
            n->data.numericLiteral.source_text_len = 0;
        } else {
            n->data.numericLiteral.source_text = p->scanner.current.text;
            n->data.numericLiteral.source_text_len = p->scanner.current.text_len;
        }
        advance(p);
        return n;
    }
    if (k == CTSC_SK_Identifier
        || (k >= CTSC_SK_BreakKeyword && k <= CTSC_SK_UnknownKeyword)) {
        return make_identifier_from_current(p);
    }
    /* Fall back to a zero-width missing Identifier (createMissingNode). */
    return make_missing_identifier(p);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeParameter (~3955):
 *     TypeParameter: (modifiers)? Identifier (`extends` Type)? (`=` Type)?
 * Modifiers are still skipped; constraint and default are consumed so the
 * scanner reaches the `>` / `,` that closes the type-parameter list (fixture
 * emitter/selfhost-derived/57_generic_constraint.ts: `T extends () => void`).
 * finishNode end = scanner.getTokenFullStart() after the constraint/default.
 */
static CtscNode* parse_type_parameter(Parser* p) {
    int pos = cur_full_start(p);
    CtscNode* name = parse_identifier(p);
    if (!name) {
        name = make_missing_identifier(p);
    }
    if (cur(p) == CTSC_SK_ExtendsKeyword) {
        advance(p);
        skip_type_in_type_parameter_position(p);
    }
    if (cur(p) == CTSC_SK_EqualsToken) {
        advance(p);
        skip_type_in_type_parameter_position(p);
    }
    int end = cur_full_start(p);
    CtscNode* tp = ctsc_node_new(p->arena, CTSC_SK_TypeParameter, pos, end);
    tp->data.typeParameter.name = name;
    return tp;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeParameters (~3987):
 *     if (token() === LessThanToken) {
 *         return parseBracketedList(TypeParameters, parseTypeParameter,
 *                                   LessThanToken, GreaterThanToken);
 *     }
 * Returns true when a `<...>` was present (and fills `out` with the parsed
 * TypeParameter nodes); false otherwise (caller leaves the list empty). The
 * fixture currently exercising this (107_FunctionPropertyAssignments6_es6.ts)
 * only hits the single-parameter, well-formed form, so a minimal
 * `<` TypeParameter (`,` TypeParameter)* `>` loop is sufficient.
 */
static bool parse_type_parameters(Parser* p, CtscNodeArray* out) {
    if (cur(p) != CTSC_SK_LessThanToken) return false;
    advance(p); /* "<" */
    if (cur(p) != CTSC_SK_GreaterThanToken) {
        for (;;) {
            int before = (int)p->scanner.pos;
            CtscNode* tp = parse_type_parameter(p);
            if (tp) {
                ctsc_node_array_push(out, p->arena, tp);
            }
            if ((int)p->scanner.pos == before) break;
            if (!accept(p, CTSC_SK_CommaToken)) break;
        }
    }
    /* Mirrors parseBracketedList which calls parseExpected(close). When the
     * `>` is missing a diagnostic is pushed without consuming. */
    if (cur(p) == CTSC_SK_GreaterThanToken) {
        advance(p);
    } else {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_GreaterThanToken));
    }
    return true;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseMethodDeclaration (~7782)
 * as invoked from parseObjectLiteralElement (~6725) for the
 *     (`*`)? PropertyName (`?`)? (`!`)? TypeParameters? `(` Parameters? `)` ReturnType? Block
 * shape. The caller has already consumed the leading asterisk (if any) and
 * parsed the PropertyName; this helper finishes the MethodDeclaration node.
 *
 * finishNode (~2600) uses end = scanner.getTokenFullStart() of the token AFTER
 * the method body, so the node's end mirrors body.end in the typical case
 * (body is a Block whose own end is finishNode-produced).
 *
 * When the method has no `(` (e.g. `{ *{ } }` from the
 * 106_FunctionPropertyAssignments3_es6.ts fixture), tsc's parseParameters
 * issues an "'(' expected." diagnostic without consuming anything and then
 * parseFunctionBlockOrSemicolon parses the body from the next `{`. ctsc
 * mirrors that recovery: we issue the diagnostic, skip parseParameters, and
 * parse the Block as-is so positions line up.
 */
static CtscNode* parse_method_declaration_rest(
    Parser* p,
    int pos,
    bool has_asterisk, int asterisk_pos, int asterisk_end,
    CtscNode* name,
    const CtscNodeArray* modifiers_opt)
{
    /* parser.ts parseMethodDeclaration (~7794): `const typeParameters =
     * parseTypeParameters();` runs between the name/questionToken and the
     * `(` of parseParameters. Fixture 107_FunctionPropertyAssignments6_es6.ts
     * (`{ *<T>() { } }`) relies on this for the MethodDeclaration's
     * `children` to include `TypeParameter` between the (missing) name and
     * the empty parameters list. */
    CtscNodeArray type_parameters; ctsc_node_array_init(&type_parameters);
    (void)parse_type_parameters(p, &type_parameters);

    CtscNodeArray params; ctsc_node_array_init(&params);
    if (cur(p) == CTSC_SK_OpenParenToken) {
        advance(p);
        if (cur(p) != CTSC_SK_CloseParenToken) {
            for (;;) {
                CtscNode* pm = parse_parameter(p);
                if (!pm) break;
                ctsc_node_array_push(&params, p->arena, pm);
                if (!accept(p, CTSC_SK_CommaToken)) break;
            }
        }
        expect(p, CTSC_SK_CloseParenToken);
    } else {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_OpenParenToken));
    }
    /* Skip optional ":" ReturnType — none of the currently-unlocked fixtures
     * exercise a typed method in an object literal, so we only consume the
     * colon when present and delegate to parse_type_annotation which handles
     * its own stop set (variables declared inside an object literal use the
     * same stop tokens). Do NOT consume when there is no colon. */
    if (cur(p) == CTSC_SK_ColonToken) {
        (void)parse_type_annotation(p);
    }
    CtscNode* body = NULL;
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        body = parse_block(p);
    } else if (cur(p) == CTSC_SK_SemicolonToken) {
        advance(p);
    }
    int end = body ? body->end : cur_full_start(p);
    CtscNode* m = ctsc_node_new(p->arena, CTSC_SK_MethodDeclaration, pos, end);
    if (modifiers_opt) {
        m->data.methodDeclaration.modifiers = *modifiers_opt;
    } else {
        ctsc_node_array_init(&m->data.methodDeclaration.modifiers);
    }
    m->data.methodDeclaration.has_asterisk = has_asterisk;
    m->data.methodDeclaration.asterisk_pos = asterisk_pos;
    m->data.methodDeclaration.asterisk_end = asterisk_end;
    m->data.methodDeclaration.name = name;
    m->data.methodDeclaration.type_parameters = type_parameters;
    m->data.methodDeclaration.parameters = params;
    m->data.methodDeclaration.body = body;
    return m;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts canFollowGetOrSetKeyword
 * (~2819): after consuming a contextual `get` / `set` keyword, the next token
 * must be either `[` (ComputedPropertyName) or a LiteralPropertyName —
 * identifier / keyword / string / numeric / bigint literal (parser.ts
 * isLiteralPropertyName ~2703). When this predicate is false, the `get` /
 * `set` token must NOT be consumed as a contextual modifier; it falls back
 * to being a regular identifier-like PropertyName (ShorthandPropertyAssignment
 * / PropertyAssignment / MethodDeclaration).
 */
static bool token_can_follow_get_or_set(CtscSyntaxKind k) {
    if (k == CTSC_SK_OpenBracketToken) return true;
    if (k == CTSC_SK_StringLiteral
        || k == CTSC_SK_NumericLiteral
        || k == CTSC_SK_BigIntLiteral) return true;
    return token_is_identifier_or_keyword(k);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseAccessorDeclaration
 * (~7851) as invoked from parseObjectLiteralElement (~6710 / ~6713) for the
 *     (`get`|`set`) PropertyName `(` Parameters? `)` ReturnType? Block
 * shape. The caller has already consumed the `get` / `set` keyword via
 * parseContextualModifier (~2754); this helper parses the property name,
 * parameter list, and body, and finishes the node.
 *
 * finishNode (~2600) uses end = scanner.getTokenFullStart() of the token
 * AFTER the body, so the node's end mirrors body.end in the typical case
 * (body is a Block whose own end is finishNode-produced). ctsc currently
 * models only `name`, `parameters`, and `body`; typeParameters, return-type
 * annotation, and modifiers are skipped until a fixture demands them.
 */
static CtscNode* parse_accessor_declaration(Parser* p, int pos, CtscSyntaxKind kind,
                                            const CtscNodeArray* modifiers_opt) {
    CtscNode* name = parse_property_name(p);

    /* parseTypeParameters runs between name and `(` in upstream. Consume
     * and discard until a fixture needs them (same treatment as in
     * parse_method_declaration_rest). */
    CtscNodeArray type_parameters; ctsc_node_array_init(&type_parameters);
    (void)parse_type_parameters(p, &type_parameters);

    CtscNodeArray params; ctsc_node_array_init(&params);
    if (cur(p) == CTSC_SK_OpenParenToken) {
        advance(p);
        if (cur(p) != CTSC_SK_CloseParenToken) {
            for (;;) {
                CtscNode* pm = parse_parameter(p);
                if (!pm) break;
                ctsc_node_array_push(&params, p->arena, pm);
                if (!accept(p, CTSC_SK_CommaToken)) break;
            }
        }
        expect(p, CTSC_SK_CloseParenToken);
    } else {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_OpenParenToken));
    }
    /* Optional `:` ReturnType — skip for now (no unlocked fixture uses a
     * typed accessor; parse_type_annotation handles its own stop set). */
    if (cur(p) == CTSC_SK_ColonToken) {
        (void)parse_type_annotation(p);
    }
    CtscNode* body = NULL;
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        body = parse_block(p);
    } else if (cur(p) == CTSC_SK_SemicolonToken) {
        advance(p);
    }
    int end = body ? body->end : cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, kind, pos, end);
    if (modifiers_opt) {
        n->data.accessorDeclaration.modifiers = *modifiers_opt;
    } else {
        ctsc_node_array_init(&n->data.accessorDeclaration.modifiers);
    }
    n->data.accessorDeclaration.name = name;
    n->data.accessorDeclaration.parameters = params;
    n->data.accessorDeclaration.body = body;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectLiteralElement
 * (~6699) reduced to the subset currently exercised by the parser curriculum:
 *   - `get` / `set` contextual modifier followed by a property name start
 *     (~6709 / ~6712) => GetAccessor / SetAccessor via
 *     parse_accessor_declaration;
 *   - optional leading `*` + PropertyName + (`(` | `<`)  => MethodDeclaration
 *     (delegated to parse_method_declaration_rest);
 *   - IdentifierLike PropertyName followed by anything other than `:` =>
 *     ShorthandPropertyAssignment (optional `= AssignmentExpression` for the
 *     CoverInitializedName grammar — see parser.ts ~6734);
 *   - otherwise: PropertyAssignment (PropertyName `:` AssignmentExpression),
 *     mirroring parser.ts ~6743. parseExpected(ColonToken) may fail (e.g. the
 *     `{ [e] }` recovery) in which case parseAssignmentExpressionOrHigher
 *     falls through to parseIdentifier(Expression_expected) producing a
 *     zero-width missing Identifier at scanner.getTokenFullStart().
 *
 * SpreadAssignment is handled first (~6703). Modifiers, questionToken,
 * exclamationToken, and JSDoc annotations are not yet modelled. When the current token cannot begin any
 * element (e.g. an unrecognised punctuator after recovery),
 * parse_object_literal_expression's progress check advances past it so the
 * outer literal's pos/end stays correct.
 */
static CtscNode* parse_object_literal_element(Parser* p) {
    int pos = cur_full_start(p);

    /* parser.ts parseObjectLiteralElement (~6703): parseOptionalToken(DotDotDotToken)
     * then parseAssignmentExpressionOrHigher. */
    if (cur(p) == CTSC_SK_DotDotDotToken) {
        advance(p);
        CtscNode* expr = parse_assignment_expression(p);
        if (!expr) {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            expr = make_missing_identifier(p);
        }
        int sa_end = cur_full_start(p);
        CtscNode* sa = ctsc_node_new(p->arena, CTSC_SK_SpreadAssignment, pos, sa_end);
        sa->data.spreadAssignment.expression = expr;
        return sa;
    }

    /* parser.ts parseObjectLiteralElement (~6709 / ~6712):
     *   if (parseContextualModifier(SyntaxKind.GetKeyword)) return parseAccessorDeclaration(..., GetAccessor, ...);
     *   if (parseContextualModifier(SyntaxKind.SetKeyword)) return parseAccessorDeclaration(..., SetAccessor, ...);
     * parseContextualModifier(~2754) is `token() === t && tryParse(nextTokenCanFollowModifier)`.
     * For GetKeyword / SetKeyword, nextTokenCanFollowModifier (~2766) does
     *   nextToken(); return canFollowGetOrSetKeyword();
     * where canFollowGetOrSetKeyword (~2819) is `OpenBracketToken || isLiteralPropertyName()`.
     * We mirror tryParse via the standard save/restore scanner pattern used
     * elsewhere in this file. */
    if (cur(p) == CTSC_SK_GetKeyword || cur(p) == CTSC_SK_SetKeyword) {
        CtscSyntaxKind accessor_kind = (cur(p) == CTSC_SK_GetKeyword)
            ? CTSC_SK_GetAccessor : CTSC_SK_SetAccessor;
        CtscScanner saved = p->scanner;
        size_t saved_diag_count = p->diagnostics->count;
        advance(p); /* consume `get` / `set` speculatively */
        if (token_can_follow_get_or_set(cur(p))) {
            return parse_accessor_declaration(p, pos, accessor_kind, NULL);
        }
        /* tryParse failed: restore and fall through to the identifier /
         * ShorthandPropertyAssignment path (e.g. `{ get: 1 }` or `{ get }`
         * where `get` is just a normal property name). */
        p->scanner = saved;
        ctsc_diag_truncate(p->diagnostics, saved_diag_count);
    }

    bool has_asterisk = false;
    int asterisk_pos = 0, asterisk_end = 0;
    if (cur(p) == CTSC_SK_AsteriskToken) {
        has_asterisk = true;
        asterisk_pos = cur_full_start(p);
        advance(p);
        /* parseTokenNode (~2553) records end = scanner.getTokenFullStart()
         * AFTER nextToken(); mirror that here. */
        asterisk_end = cur_full_start(p);
    }

    /* parser.ts ~6717: capture `isIdentifier()` BEFORE parsePropertyName so
     * ShorthandPropertyAssignment is only taken when the property-name token
     * itself was identifier-like (not a computed name, string, or numeric). */
    bool token_was_identifier = token_is_identifier_expression(cur(p));

    /* parsePropertyName — identifier / keyword / literal / computed / missing. */
    CtscNode* name = parse_property_name(p);

    /* Per parser.ts ~6724, asteriskToken OR `(` OR `<` routes to
     * MethodDeclaration. `<` introduces TypeParameters which
     * parse_method_declaration_rest consumes before parseParameters. */
    if (has_asterisk || cur(p) == CTSC_SK_OpenParenToken || cur(p) == CTSC_SK_LessThanToken) {
        return parse_method_declaration_rest(p, pos,
            has_asterisk, asterisk_pos, asterisk_end, name, NULL);
    }

    /* parser.ts ~6734: `tokenIsIdentifier && token() !== ColonToken` routes
     * to ShorthandPropertyAssignment, with an optional `= AssignmentExpression`
     * tail covering the CoverInitializedName grammar. Everything else is a
     * PropertyAssignment with a required `:` and an AssignmentExpression. */
    bool is_shorthand = token_was_identifier && cur(p) != CTSC_SK_ColonToken;
    if (is_shorthand) {
        CtscNode* init = NULL;
        if (accept(p, CTSC_SK_EqualsToken)) {
            init = parse_assignment_expression(p);
            if (!init) {
                ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                    cur_start(p), cur_end(p) - cur_start(p),
                    "Expression expected.");
                init = make_missing_identifier(p);
            }
        }
        int end = init ? init->end : cur_full_start(p);
        CtscNode* sp = ctsc_node_new(p->arena, CTSC_SK_ShorthandPropertyAssignment, pos, end);
        sp->data.shorthandPropertyAssignment.name = name;
        sp->data.shorthandPropertyAssignment.objectAssignmentInitializer = init;
        return sp;
    }

    /* PropertyAssignment: `PropertyName : AssignmentExpression`. When the
     * colon is missing (e.g. `{ [e] }`), parseExpected pushes a diagnostic
     * without consuming; parseAssignmentExpressionOrHigher then falls through
     * to parseIdentifier(Expression_expected) which synthesises a zero-width
     * missing Identifier at scanner.getTokenFullStart(). */
    expect(p, CTSC_SK_ColonToken);
    CtscNode* init = parse_assignment_expression(p);
    if (!init) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        init = make_missing_identifier(p);
    }
    /* finishNode(~2600) uses end = scanner.getTokenFullStart() of the token
     * AFTER the initializer. init->end is normally equal to cur_full_start(p)
     * right after parseAssignmentExpressionOrHigher returns, but using the
     * scanner directly keeps us aligned with the `parseExpected(ColonToken)
     * fails + missing initializer` recovery path where init is a zero-width
     * missing Identifier at cur_full_start. */
    int pa_end = cur_full_start(p);
    CtscNode* pa = ctsc_node_new(p->arena, CTSC_SK_PropertyAssignment, pos, pa_end);
    pa->data.propertyAssignment.name = name;
    pa->data.propertyAssignment.initializer = init;
    return pa;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isListElement (~2866) for
 * ParsingContext.ObjectLiteralMembers: a property-name-start token, or one of
 * `[` (ComputedPropertyName), `*` (generator method), `...` (SpreadAssignment),
 * or `.` (the "don't close the object on a stray dot" recovery case noted at
 * parser.ts ~2871). Otherwise the element starts with an isLiteralPropertyName
 * token: identifier / keyword / string / numeric / bigint literal (parser.ts
 * isLiteralPropertyName ~2703).
 */
static bool is_object_literal_member_start(CtscSyntaxKind k) {
    if (k == CTSC_SK_OpenBracketToken
        || k == CTSC_SK_AsteriskToken
        || k == CTSC_SK_DotDotDotToken
        || k == CTSC_SK_DotToken) {
        return true;
    }
    if (k == CTSC_SK_StringLiteral
        || k == CTSC_SK_NumericLiteral
        || k == CTSC_SK_BigIntLiteral) {
        return true;
    }
    return token_is_identifier_or_keyword(k);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectLiteralExpression
 * (~6755): `{` parseDelimitedList(ObjectLiteralMembers, parseObjectLiteralElement,
 * considerSemicolonAsDelimiter=true) `}`. finishNode sets
 * end = scanner.getTokenFullStart() of the token AFTER the closing `}` which
 * equals cur_full_start(p) right after expect(CloseBraceToken) consumes it.
 *
 * The delimited-list loop mirrors parser.ts parseDelimitedList (~3492):
 *   while true:
 *       if isListElement(ObjectLiteralMembers):
 *           parse element;
 *           if parseOptional(CommaToken) continue;
 *           if isListTerminator(ObjectLiteralMembers) - `}` or EOF
 *               break;
 *           parseExpected(CommaToken);  // pushes diagnostic, no consume
 *           if considerSemicolonAsDelimiter
 *              AND token() === `;`
 *              AND !scanner.hasPrecedingLineBreak(): nextToken();
 *           if no progress made: nextToken();
 *           continue;
 *       if isListTerminator: break;
 *       if abortParsingListOrMoveToNextToken: break;
 *
 * abortParsingListOrMoveToNextToken is guaranteed to return true here because
 * we are always nested in at least the SourceElements parsing context, so
 * isInSomeParsingContext() is true; bailing lets the outer list handle the
 * stray token (e.g. `;` starts an EmptyStatement).
 *
 * Without the considerSemicolonAsDelimiter path plus the bad-start abort,
 * ctsc synthesised a spurious empty PropertyAssignment at the stray token's
 * position. See fixture 108_parserErrorRecovery_ObjectLiteral2.ts
 * (`var v = { a\nreturn;`) where the trailing `;` after the recovered `return`
 * property would otherwise seed a second (39,39) PropertyAssignment instead
 * of being consumed as a comma-replacement delimiter by the loop.
 */
static CtscNode* parse_object_literal_expression(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* "{" */
    /* Mirrors parser.ts parseObjectLiteralExpression (~6759). */
    bool multi_line = p->scanner.current.has_preceding_line_break;
    bool has_trailing_comma = false;
    CtscNodeArray properties; ctsc_node_array_init(&properties);
    while (cur(p) != CTSC_SK_EndOfFileToken && cur(p) != CTSC_SK_CloseBraceToken) {
        if (is_object_literal_member_start(cur(p))) {
            int start_full_start = cur_full_start(p);
            CtscNode* elt = parse_object_literal_element(p);
            if (elt) {
                ctsc_node_array_push(&properties, p->arena, elt);
            }
            if (accept(p, CTSC_SK_CommaToken)) {
                if (cur(p) == CTSC_SK_CloseBraceToken) has_trailing_comma = true;
                continue;
            }
            if (cur(p) == CTSC_SK_EndOfFileToken || cur(p) == CTSC_SK_CloseBraceToken) break;
            /* parseExpected(CommaToken): diagnostic, no consume. */
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
                cur_start(p), cur_end(p) - cur_start(p),
                "',' expected.");
            /* considerSemicolonAsDelimiter=true at parser.ts ~6760. */
            if (cur(p) == CTSC_SK_SemicolonToken
                && !p->scanner.current.has_preceding_line_break) {
                advance(p);
            }
            if (cur_full_start(p) == start_full_start) {
                /* No progress — skip one token to avoid an infinite loop
                 * (parser.ts ~3532). */
                advance(p);
            }
            continue;
        }
        /* Current token cannot start an ObjectLiteralMember. ObjectLiteralMembers'
         * only terminator is CloseBrace (or EOF); both are handled by the outer
         * while condition. For any other stray token, mirror
         * abortParsingListOrMoveToNextToken (~3410): we're always inside at
         * least the SourceElements parsing context, so isInSomeParsingContext()
         * is true and the delimited list is aborted (return to the outer
         * context). This matches tsc on `var v = { a\nreturn;` where the
         * trailing `;` ends the ObjectLiteral without seeding a new property.
         * We do NOT advance; the outer statement loop will (e.g. SemicolonToken
         * → EmptyStatement, unrecognised token → error + skip). */
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1136,
            cur_start(p), cur_end(p) - cur_start(p),
            "Property assignment expected.");
        break;
    }
    expect(p, CTSC_SK_CloseBraceToken);
    int end = cur_full_start(p);
    CtscNode* obj = ctsc_node_new(p->arena, CTSC_SK_ObjectLiteralExpression, fs, end);
    obj->data.objectLiteralExpression.properties = properties;
    obj->data.objectLiteralExpression.multi_line = multi_line;
    obj->data.objectLiteralExpression.has_trailing_comma = has_trailing_comma;
    return obj;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isBindingIdentifier (~2308) for the
 * keyword-range rule: Identifier, or token > LastReservedWord (ctsc: reserved Break..With,
 * then contextual / TS keywords As..Unknown).
 */
static bool is_binding_identifier_kind(CtscSyntaxKind k) {
    if (k == CTSC_SK_Identifier) return true;
    if (k >= CTSC_SK_BreakKeyword && k <= CTSC_SK_WithKeyword) return false;
    if (k >= CTSC_SK_AsKeyword && k <= CTSC_SK_UnknownKeyword) return true;
    return false;
}

/*
 * Mirrors parser.ts nextTokenIsBindingIdentifierOrStartOfDestructuring (~7331) +
 * isLetDeclaration (~7336).
 */
static bool is_let_declaration(Parser* p) {
    CtscScanner saved = p->scanner;
    advance(p);
    CtscSyntaxKind next = cur(p);
    p->scanner = saved;
    if (next == CTSC_SK_OpenBraceToken || next == CTSC_SK_OpenBracketToken) return true;
    return is_binding_identifier_kind(next);
}

/*
 * Mirrors parser.ts isIdentifier (~2318) for primary expressions: real identifiers,
 * this/true/false/null, and contextual keywords (token > LastReservedWord).
 */
static bool token_is_identifier_expression(CtscSyntaxKind k) {
    if (k == CTSC_SK_Identifier) return true;
    if (k == CTSC_SK_ThisKeyword || k == CTSC_SK_TrueKeyword || k == CTSC_SK_FalseKeyword || k == CTSC_SK_NullKeyword) {
        return true;
    }
    if (k >= CTSC_SK_BreakKeyword && k <= CTSC_SK_WithKeyword) return false;
    if (k >= CTSC_SK_AsKeyword && k <= CTSC_SK_UnknownKeyword) return true;
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeLiteral (~4373):
 *     TypeLiteral: "{" TypeMember* "}"
 * and parseObjectTypeMembers (~4378). ctsc does not yet model TypeElement
 * members (PropertySignature / MethodSignature / ...); we brace-balance the
 * body so the outer node's pos/end line up with tsc's finishNode (end =
 * scanner.getTokenFullStart() of the token AFTER the closing brace, which
 * equals cur_full_start after consuming "}"). The oracle
 * (harness/src/oracle-ast.ts) has no explicit case for TypeLiteral and
 * VariableDeclaration's case intentionally omits the `type` field (see its
 * ts.SyntaxKind.VariableDeclaration branch), so ctsc does not need to
 * serialize anything for this node to achieve byte-exact parity for the
 * surrounding declaration's end position.
 */
static CtscNode* parse_type_literal(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* "{" */
    int depth = 1;
    while (depth > 0 && cur(p) != CTSC_SK_EndOfFileToken) {
        if (cur(p) == CTSC_SK_OpenBraceToken) { depth++; advance(p); continue; }
        if (cur(p) == CTSC_SK_CloseBraceToken) {
            depth--;
            if (depth == 0) break;
            advance(p);
            continue;
        }
        advance(p);
    }
    expect(p, CTSC_SK_CloseBraceToken);
    int end = cur_full_start(p);
    return ctsc_node_new(p->arena, CTSC_SK_TypeLiteral, fs, end);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parsePostfixTypeOrHigher
 * (~4716): after parseNonArrayType(), consume repeated postfix `[` ... `]`
 * (array type `T[]` or indexed access `T[K]`). Without this, `...nums: number[]`
 * leaves `[` unconsumed and the parameter-list loop treats `[]` as a second
 * Parameter (ArrayBindingPattern) — emitter/selfhost-derived/60_rest_parameters.ts.
 */
static void consume_postfix_type_operators(Parser* p) {
    while (!p->scanner.current.has_preceding_line_break
           && cur(p) == CTSC_SK_OpenBracketToken) {
        advance(p); /* `[` */
        if (cur(p) != CTSC_SK_CloseBracketToken && token_can_start_type(cur(p))) {
            skip_type_in_type_parameter_position(p);
        }
        if (cur(p) == CTSC_SK_CloseBracketToken) {
            advance(p);
        } else {
            break;
        }
    }
}

/* Stop-set scan for types that are not modelled as full TypeNodes yet.
 * Leading `{ ... }` unions call this after each `|` when the RHS is not an
 * object type literal (mirrors parser.ts parseUnionType ~4876). */
static CtscNode* consume_type_via_fallback_scan(Parser* p, bool allow_multiline) {
    /* Unknown (e.g. a stray '\\') always terminates at any depth: mirrors tsc's
     * parseTypeReference flow for `var v: X<T \\` (106_parserSkippedTokens20.ts). */
    int fs = cur_full_start(p);
    int brace_depth = 0;
    int bracket_depth = 0;
    int paren_depth = 0;
    int angle_depth = 0;
    bool after_top_level_bracket = false;
    CtscSyntaxKind last_consumed_kind = CTSC_SK_Unknown;
    while (cur(p) != CTSC_SK_EndOfFileToken) {
        CtscSyntaxKind k = cur(p);
        if (k == CTSC_SK_Unknown) break;
        if (brace_depth == 0 && bracket_depth == 0 && paren_depth == 0 && angle_depth == 0) {
            if (after_top_level_bracket && k == CTSC_SK_OpenBraceToken) {
                break;
            }
            if (after_top_level_bracket && k != CTSC_SK_OpenBraceToken) {
                after_top_level_bracket = false;
            }
            if (k == CTSC_SK_EqualsToken || k == CTSC_SK_CommaToken
                || k == CTSC_SK_SemicolonToken
                || k == CTSC_SK_CloseParenToken || k == CTSC_SK_CloseBracketToken
                || k == CTSC_SK_CloseBraceToken
                || (k == CTSC_SK_OpenBraceToken && last_consumed_kind != CTSC_SK_BarToken)) {
                break;
            }
            if (p->scanner.current.has_preceding_line_break && !allow_multiline) break;
        }
        if (k == CTSC_SK_OpenBraceToken) brace_depth++;
        else if (k == CTSC_SK_CloseBraceToken) { if (brace_depth == 0) break; brace_depth--; }
        else if (k == CTSC_SK_OpenBracketToken) bracket_depth++;
        else if (k == CTSC_SK_CloseBracketToken) {
            if (bracket_depth == 0) break;
            bracket_depth--;
            if (bracket_depth == 0) after_top_level_bracket = true;
        }
        else if (k == CTSC_SK_OpenParenToken) paren_depth++;
        else if (k == CTSC_SK_CloseParenToken) { if (paren_depth == 0) break; paren_depth--; }
        else if (k == CTSC_SK_LessThanToken) angle_depth++;
        else if (k == CTSC_SK_GreaterThanToken && angle_depth > 0) angle_depth--;
        advance(p);
        last_consumed_kind = k;
    }
    int end = cur_full_start(p);
    return ctsc_node_new(p->arena, CTSC_SK_TypeReference, fs, end);
}

/*
 * Parses a Type after `:` (annotations) or `=` (type aliases). When
 * `allow_multiline` is false, a line break at the top level terminates the
 * type (ASI / variable-declaration parity). When true, multiline union types
 * after `=` are absorbed (mirrors parser.ts parseTypeAliasDeclaration ~8257
 * feeding parseType, selfhost-derived 89_type_alias_erase.ts).
 *
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeAnnotation (~4961)
 * for the `:` case: TypeAnnotation: ":" Type
 */
static CtscNode* parse_type_in_annotation_position(Parser* p, bool allow_multiline) {
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        CtscNode* tl = parse_type_literal(p);
        consume_postfix_type_operators(p);
        /* Union of object types (`{ a: 1 } | { b: 2 }`) — must not stop after
         * the first `}` or the function `{` body is misparsed as a top-level
         * block (emitter/selfhost-derived/90_union_return_type_object.ts). */
        while (!p->scanner.current.has_preceding_line_break
               && cur(p) == CTSC_SK_BarToken) {
            advance(p); /* `|` */
            if (cur(p) == CTSC_SK_OpenBraceToken) {
                (void)parse_type_literal(p);
                consume_postfix_type_operators(p);
            } else {
                (void)consume_type_via_fallback_scan(p, allow_multiline);
            }
        }
        tl->end = cur_full_start(p);
        return tl;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseNonArrayType (~4629):
     *     case SyntaxKind.VoidKeyword:
     *         return parseTokenNode<TypeNode>();
     * Unlike the other keyword types (any/number/string/...), `void` does NOT
     * fall through tryParse(parseKeywordAndNoDot) || parseTypeReference(), so
     * a following `.` is NOT consumed as a dotted type reference — the `void`
     * node ends at scanner.getTokenFullStart() of the `.` token. Fixture
     * 106_parservoidInQualifiedName1.ts (`var v : void.x;`) relies on this:
     * the VariableDeclaration ends at full_start(`.`) and the stray `.x` is
     * then absorbed by parseDelimitedList's recovery (see parse_variable_statement
     * below). */
    if (cur(p) == CTSC_SK_VoidKeyword) {
        int fs = cur_full_start(p);
        CtscScanner saved = p->scanner;
        advance(p);
        /* Union types (`void | null`) must use the stop-set fallback so `|`
         * and the RHS are absorbed (parser.ts parseUnionType ~4876). */
        if (cur(p) != CTSC_SK_BarToken) {
            consume_postfix_type_operators(p);
            int end = cur_full_start(p);
            return ctsc_node_new(p->arena, CTSC_SK_VoidKeyword, fs, end);
        }
        p->scanner = saved;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseNonArrayType
     * (~4591-4602):
     *     case SyntaxKind.AnyKeyword:
     *     case SyntaxKind.UnknownKeyword:
     *     case SyntaxKind.StringKeyword:
     *     case SyntaxKind.NumberKeyword:
     *     case SyntaxKind.BigIntKeyword:
     *     case SyntaxKind.SymbolKeyword:
     *     case SyntaxKind.BooleanKeyword:
     *     case SyntaxKind.UndefinedKeyword:
     *     case SyntaxKind.NeverKeyword:
     *     case SyntaxKind.ObjectKeyword:
     *         // If these are followed by a dot, then parse these out as a
     *         // dotted type reference instead.
     *         return tryParse(parseKeywordAndNoDot) || parseTypeReference();
     * parseKeywordAndNoDot (~4523) consumes the keyword as a single-token
     * TypeNode when the next token is not `.`. We only need to advance the
     * scanner past the keyword for position parity — the oracle does not
     * serialise FunctionDeclaration/VariableDeclaration `type`, so the
     * returned node's kind does not affect JSON output. The `.`-recovery
     * fallback (parseTypeReference) is absorbed by the generic
     * stop-set loop below, which permits `a.b.c` trailing tokens.
     *
     * Without this, the stop-set loop swallows e.g. `any { }` (the `{`
     * opens a brace depth, then `}` closes it) and the FunctionDeclaration
     * loses its body. Fixture 107_generatorTypeCheck5.ts
     * (`function* g1(): any { }`) exercises this. */
    {
        CtscSyntaxKind tk = cur(p);
        if (tk == CTSC_SK_AnyKeyword || tk == CTSC_SK_UnknownKeyword
            || tk == CTSC_SK_StringKeyword || tk == CTSC_SK_NumberKeyword
            || tk == CTSC_SK_SymbolKeyword || tk == CTSC_SK_BooleanKeyword
            || tk == CTSC_SK_UndefinedKeyword || tk == CTSC_SK_ObjectKeyword
            || tk == CTSC_SK_NeverKeyword) {
            /* tryParse(parseKeywordAndNoDot): peek the next token — if it's
             * `.`, fall through to the generic fallback (which will absorb
             * the dotted TypeReference). Otherwise consume as a single-token
             * keyword-typed TypeNode. */
            int fs = cur_full_start(p);
            CtscScanner saved = p->scanner;
            advance(p);
            bool next_is_dot = cur(p) == CTSC_SK_DotToken;
            if (!next_is_dot) {
                if (cur(p) == CTSC_SK_BarToken) {
                    p->scanner = saved;
                } else {
                    consume_postfix_type_operators(p);
                    /* Union after postfix (`string[] | undefined`) must use the
                     * stop-set fallback so `| ...` is absorbed (parser.ts
                     * parseUnionType ~4876). Returning here left `| undefined`
                     * unconsumed; the parameter list then parsed `undefined` as
                     * a second Parameter (selfhost-derived optional-chain export
                     * fixture). */
                    if (cur(p) == CTSC_SK_BarToken) {
                        p->scanner = saved;
                    } else {
                        int end = cur_full_start(p);
                        return ctsc_node_new(p->arena, tk, fs, end);
                    }
                }
            } else {
                p->scanner = saved;
            }
        }
    }
    /* Identifier-led TypeReference: mirror upstream parser.ts parseTypeReference
     * (~4577) → parseEntityNameOfTypeReference + parseTypeArgumentsOfTypeReference.
     * parse_type_node already produces a CTSC_SK_TypeReference carrying
     * typeName=Identifier (+ optional typeArguments), which
     * forEachChildInTypeReference visits — necessary for
     * PropertyDeclaration.type to serialise its children correctly
     * (fixture 108_parserComputedPropertyName9.ts). Fall back to the
     * stop-set scanner when the identifier is followed by unsupported
     * type syntax (e.g. `X | Y`, `X[]`, `X.Y`): we speculatively call
     * parse_type_node and accept it only when the next token is a
     * top-level terminator that the stop-set path would also have
     * stopped on, preserving finishNode.end for existing fixtures. */
    if (cur(p) == CTSC_SK_Identifier) {
        CtscScanner saved = p->scanner;
        size_t saved_diag_count = p->diagnostics->count;
        CtscNode* tr = parse_type_node(p);
        if (tr) {
            consume_postfix_type_operators(p);
            tr->end = cur_full_start(p);
            CtscSyntaxKind nk = cur(p);
            bool terminates =
                (nk == CTSC_SK_EqualsToken || nk == CTSC_SK_CommaToken
                 || nk == CTSC_SK_SemicolonToken
                 || nk == CTSC_SK_CloseParenToken || nk == CTSC_SK_CloseBracketToken
                 || nk == CTSC_SK_CloseBraceToken
                 || nk == CTSC_SK_OpenBraceToken
                 || nk == CTSC_SK_EqualsGreaterThanToken
                 || nk == CTSC_SK_EndOfFileToken)
                || (p->scanner.current.has_preceding_line_break && !allow_multiline);
            if (terminates) return tr;
        }
        p->scanner = saved;
        ctsc_diag_truncate(p->diagnostics, saved_diag_count);
    }
    /* Fallback: consume a minimal "type expression" by scanning a stop set
     * (nested `{`/`[` balanced; see consume_type_via_fallback_scan). */
    return consume_type_via_fallback_scan(p, allow_multiline);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeAnnotation (~4961).
 * Returns NULL when no ":" is present.
 */
static CtscNode* parse_type_annotation(Parser* p) {
    if (cur(p) != CTSC_SK_ColonToken) return NULL;
    advance(p); /* ":" */
    return parse_type_in_annotation_position(p, false);
}

/*
 * Consumes one Type after `extends` or `=` in a TypeParameterDeclaration.
 * Mirrors the fallback branch of parse_type_annotation (~2552): brace/bracket/
 * paren/angle-balanced scan until a top-level delimiter.
 */
static void skip_type_in_type_parameter_position(Parser* p) {
    int brace_depth = 0;
    int bracket_depth = 0;
    int paren_depth = 0;
    int angle_depth = 0;
    while (cur(p) != CTSC_SK_EndOfFileToken) {
        CtscSyntaxKind k = cur(p);
        if (k == CTSC_SK_Unknown) break;
        if (brace_depth == 0 && bracket_depth == 0 && paren_depth == 0 && angle_depth == 0) {
            if (k == CTSC_SK_EqualsToken || k == CTSC_SK_CommaToken
                || k == CTSC_SK_SemicolonToken
                || k == CTSC_SK_CloseParenToken || k == CTSC_SK_CloseBracketToken
                || k == CTSC_SK_CloseBraceToken
                || k == CTSC_SK_GreaterThanToken) {
                break;
            }
            if (p->scanner.current.has_preceding_line_break) break;
        }
        if (k == CTSC_SK_OpenBraceToken) brace_depth++;
        else if (k == CTSC_SK_CloseBraceToken) { if (brace_depth == 0) break; brace_depth--; }
        else if (k == CTSC_SK_OpenBracketToken) bracket_depth++;
        else if (k == CTSC_SK_CloseBracketToken) { if (bracket_depth == 0) break; bracket_depth--; }
        else if (k == CTSC_SK_OpenParenToken) paren_depth++;
        else if (k == CTSC_SK_CloseParenToken) { if (paren_depth == 0) break; paren_depth--; }
        else if (k == CTSC_SK_LessThanToken) angle_depth++;
        else if (k == CTSC_SK_GreaterThanToken && angle_depth > 0) angle_depth--;
        advance(p);
    }
}

static CtscNode* parse_variable_declaration(Parser* p) {
    int fs = cur_full_start(p);
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseVariableDeclaration
     * (~7649): name = parseIdentifierOrPattern(...); the name is a
     * BindingIdentifier (contextual/TS keywords allowed) or a binding
     * pattern. Previously used parse_identifier, which rejected `var of`
     * — needed for fixtures/emitter/from-upstream/107_parserForOfStatement17.ts
     * (`for (var of; ;) { }`). */
    CtscNode* name = parse_identifier_or_pattern(p);
    if (!name) return NULL;
    int end = name->end;
    /* name (exclamation?) (typeAnnotation?) (initializer?). Skipping the
     * optional exclamation-for-definite-assignment for now; no current
     * fixture exercises `var x!: T`. */
    CtscNode* type = parse_type_annotation(p);
    if (type) end = type->end;
    CtscNode* init = NULL;
    if (accept(p, CTSC_SK_EqualsToken)) {
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseInitializer
         * (~5065): parseOptional(EqualsToken) ?
         *     parseAssignmentExpressionOrHigher(allowReturnTypeInArrowFunction=true)
         *     : undefined. The comma operator is NOT allowed at the top of a
         *     declarator initializer: in `var a = 1, b = 2` the `,` is the
         *     declarator separator, not a comma expression. */
        init = parse_assignment_expression(p);
        if (init) end = init->end;
    }
    CtscNode* decl = ctsc_node_new(p->arena, CTSC_SK_VariableDeclaration, fs, end);
    decl->data.variableDeclaration.name = name;
    decl->data.variableDeclaration.type = type;
    decl->data.variableDeclaration.initializer = init;
    return decl;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts canParseSemicolon (~2567):
 *   explicit ';'  |  '}'  |  EOF  |  scanner.hasPrecedingLineBreak()
 * Used to detect ASI points so statement-level recovery can terminate the
 * VariableDeclarations delimited list without consuming arbitrary trailing
 * junk.
 */
static bool can_parse_semicolon_here(const Parser* p) {
    if (cur(p) == CTSC_SK_SemicolonToken) return true;
    return cur(p) == CTSC_SK_CloseBraceToken
        || cur(p) == CTSC_SK_EndOfFileToken
        || p->scanner.current.has_preceding_line_break;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isBindingIdentifier (~2308)
 * combined with isBindingIdentifierOrPrivateIdentifierOrPattern (~7628) used
 * by parseDelimitedList(VariableDeclarations).isListElement (~2898). Returns
 * true when the current token can start a variable declarator's binding name:
 * a real Identifier, a contextual/TS keyword (> LastReservedWord), or the
 * opening of an array/object binding pattern. ctsc does not yet tokenise
 * PrivateIdentifier as a distinct kind; that branch is omitted but harmless
 * until a fixture demands it.
 */
static bool is_binding_identifier_start(CtscSyntaxKind k) {
    if (k == CTSC_SK_OpenBracketToken || k == CTSC_SK_OpenBraceToken) return true;
    if (k == CTSC_SK_Identifier) return true;
    /* token() > LastReservedWord: contextual keywords are valid binding names. */
    if (k >= CTSC_SK_AsKeyword && k <= CTSC_SK_UnknownKeyword) return true;
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isVariableDeclaratorListTerminator
 * (~3052): the VariableDeclarations delimited list is done when we can place a
 * semicolon (explicit / ASI / `}` / EOF), or when we see an `in` / `of`
 * keyword (for-in/of head), or `=>` (arrow-function error-recovery shortcut).
 */
static bool is_variable_declarator_list_terminator(const Parser* p) {
    if (can_parse_semicolon_here(p)) return true;
    if (cur(p) == CTSC_SK_InKeyword || cur(p) == CTSC_SK_OfKeyword) return true;
    if (cur(p) == CTSC_SK_EqualsGreaterThanToken) return true;
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isStartOfStatement (~7268)
 * restricted to the token-level check used by
 * abortParsingListOrMoveToNextToken's outer SourceElements claim (~2842 +
 * ~7268). When a VariableDeclarations delimited list encounters an unexpected
 * token, tsc advances the scanner only when NO outer parsing context claims
 * it; for the `var ... /[]/]/` fixture (106_parser645086_1.ts) that means the
 * trailing `]` is consumed inside parseDelimitedList (because SourceElements
 * does not claim it) but the following `/` stops recovery (SourceElements'
 * isStartOfStatement accepts SlashToken via isStartOfLeftHandSideExpression).
 *
 * ctsc does not yet model parsing contexts as a bitmask, so this helper
 * approximates SourceElements.isListElement by returning true for tokens that
 * clearly start a statement or expression. The set mirrors
 * isStartOfStatement + isStartOfExpression + isStartOfLeftHandSideExpression
 * for the kinds ctsc currently tokenises; kinds we do not yet emit are simply
 * absent (they will be added as fixtures demand).
 */
static bool is_start_of_statement_token(const Parser* p) {
    CtscSyntaxKind k = cur(p);
    switch (k) {
        case CTSC_SK_AtToken:
        case CTSC_SK_SemicolonToken:
        case CTSC_SK_OpenBraceToken:
        case CTSC_SK_VarKeyword:
        case CTSC_SK_LetKeyword:
        case CTSC_SK_ConstKeyword:
        case CTSC_SK_FunctionKeyword:
        case CTSC_SK_ClassKeyword:
        case CTSC_SK_EnumKeyword:
        case CTSC_SK_IfKeyword:
        case CTSC_SK_DoKeyword:
        case CTSC_SK_WhileKeyword:
        case CTSC_SK_ForKeyword:
        case CTSC_SK_ContinueKeyword:
        case CTSC_SK_BreakKeyword:
        case CTSC_SK_ReturnKeyword:
        case CTSC_SK_WithKeyword:
        case CTSC_SK_SwitchKeyword:
        case CTSC_SK_ThrowKeyword:
        case CTSC_SK_TryKeyword:
        case CTSC_SK_CatchKeyword:
        case CTSC_SK_FinallyKeyword:
        case CTSC_SK_DebuggerKeyword:
        case CTSC_SK_ImportKeyword:
        case CTSC_SK_ExportKeyword:
        case CTSC_SK_AsyncKeyword:
        case CTSC_SK_DeclareKeyword:
        case CTSC_SK_InterfaceKeyword:
        case CTSC_SK_ModuleKeyword:
        case CTSC_SK_NamespaceKeyword:
        case CTSC_SK_TypeKeyword:
        /* isStartOfLeftHandSideExpression (~4966): primary-expression starters. */
        case CTSC_SK_ThisKeyword:
        case CTSC_SK_SuperKeyword:
        case CTSC_SK_NullKeyword:
        case CTSC_SK_TrueKeyword:
        case CTSC_SK_FalseKeyword:
        case CTSC_SK_NumericLiteral:
        case CTSC_SK_BigIntLiteral:
        case CTSC_SK_StringLiteral:
        case CTSC_SK_NoSubstitutionTemplateLiteral:
        case CTSC_SK_TemplateHead:
        case CTSC_SK_OpenParenToken:
        case CTSC_SK_OpenBracketToken:
        case CTSC_SK_NewKeyword:
        case CTSC_SK_SlashToken:
        case CTSC_SK_SlashEqualsToken:
        case CTSC_SK_Identifier:
        /* isStartOfExpression (~4995): prefix-unary starters & keyword exprs. */
        case CTSC_SK_PlusToken:
        case CTSC_SK_MinusToken:
        case CTSC_SK_TildeToken:
        case CTSC_SK_ExclamationToken:
        case CTSC_SK_DeleteKeyword:
        case CTSC_SK_TypeOfKeyword:
        case CTSC_SK_VoidKeyword:
        case CTSC_SK_PlusPlusToken:
        case CTSC_SK_MinusMinusToken:
        case CTSC_SK_LessThanToken:
        case CTSC_SK_AwaitKeyword:
        case CTSC_SK_YieldKeyword:
            return true;
        default:
            /* Fall back to contextual/TS keywords that act as identifiers in
             * expression position (parser.ts isIdentifier ~2318). */
            if (token_is_identifier_expression(k)) return true;
            return false;
    }
}

static CtscNode* parse_variable_statement(Parser* p) {
    int fs = cur_full_start(p);
    int flags = 0;
    if      (cur(p) == CTSC_SK_LetKeyword)   { flags |= 1; }
    else if (cur(p) == CTSC_SK_ConstKeyword) { flags |= 2; }
    /* else var */
    int list_fs = cur_full_start(p);
    advance(p); /* var/let/const */
    CtscNodeArray decls; ctsc_node_array_init(&decls);
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseDelimitedList
     * (~3489) for ParsingContext.VariableDeclarations, with isListElement
     * (~2898 VariableDeclarations → isBindingIdentifierOrPrivateIdentifierOrPattern),
     * isListTerminator (~3021 → isVariableDeclaratorListTerminator ~3052),
     * and abortParsingListOrMoveToNextToken (~3410) faithfully modelled for
     * the tokens ctsc currently emits. Three scenarios drive this shape:
     *
     *   1. `var a, b;`  — comma-delimited, terminator is `;` / ASI / `}` / EOF
     *      / `in` / `of` / `=>`.
     *   2. `var v = /[]/]/`  (106_parser645086_1.ts) — after the first decl's
     *      initializer, the stray `]` is NOT an element start and NOT a
     *      terminator, but SourceElements (the outer context) doesn't claim
     *      it either; we advance past it. The following `/` IS a start of
     *      statement (SlashToken begins an LHS expression), so SourceElements
     *      claims it and we break, leaving list_end = full_start(`/`) = 32.
     *   3. `var v : void.x;`  (106_parservoidInQualifiedName1.ts) — after the
     *      first decl ends at `.` (see VoidKeyword handling in
     *      parse_type_annotation), `.` is not an element/terminator, outer
     *      SourceElements doesn't claim it → advance. Next token `x` IS a
     *      valid binding identifier → parse a SECOND VariableDeclaration
     *      (pos=32, end=33). Then `;` is a terminator, loop breaks. */
    for (;;) {
        if (is_binding_identifier_start(cur(p))) {
            int el_fs = cur_full_start(p);
            CtscNode* d = parse_variable_declaration(p);
            if (!d) break;
            ctsc_node_array_push(&decls, p->arena, d);
            if (accept(p, CTSC_SK_CommaToken)) continue;
            if (is_variable_declarator_list_terminator(p)) break;
            /* parseExpected(CommaToken) emits an error without advancing.
             * We don't emit the diagnostic here (oracle doesn't inspect
             * parse diagnostics), but we mirror the zero-progress guard
             * (~3532) to avoid an infinite loop when the decl consumed no
             * tokens — which cannot actually happen here since
             * parse_variable_declaration only succeeds after parse_identifier
             * advances — but keep the guard defensively. */
            if (el_fs == cur_full_start(p)) advance(p);
            continue;
        }
        if (is_variable_declarator_list_terminator(p)) break;
        /* abortParsingListOrMoveToNextToken (~3410): if an outer parsing
         * context (SourceElements in our current call tree) claims this
         * token as a list element or terminator, break and let the outer
         * context handle it. ctsc approximates SourceElements.isListElement
         * with is_start_of_statement_token; this is sufficient because
         * parseVariableStatement is only ever called from parseStatement
         * within SourceElements. */
        if (is_start_of_statement_token(p)) break;
        if (cur(p) == CTSC_SK_EndOfFileToken) break;
        advance(p);
    }
    /* Mirrors parser.ts finishNode (~2600): end defaults to
     * scanner.getTokenFullStart() of the token AFTER the list, which equals
     * the end of the last consumed non-trivia character. In the empty-list
     * case (e.g. `var ;`) this is the end of the var/let/const keyword. */
    int list_end = cur_full_start(p);
    CtscNode* list = ctsc_node_new(p->arena, CTSC_SK_VariableDeclarationList, list_fs, list_end);
    list->data.variableDeclarationList.flags = flags;
    list->data.variableDeclarationList.declarations = decls;
    int stmt_end = list_end;
    if (cur(p) == CTSC_SK_SemicolonToken) { stmt_end = cur_end(p); advance(p); }
    CtscNode* stmt = ctsc_node_new(p->arena, CTSC_SK_VariableStatement, fs, stmt_end);
    stmt->data.variableStatement.declarationList = list;
    return stmt;
}

static CtscNode* parse_statement(Parser* p);
static CtscNode* parse_expression_or_labeled_statement(Parser* p, int stmt_start);

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseClassElement
 * (~8068) reduced to the subset currently exercised by the parser curriculum:
 *   - `;`               => SemicolonClassElement (parser.ts ~8071);
 *   - leading `*` + PropertyName => MethodDeclaration via parsePropertyOrMethodDeclaration
 *     (~7835) → parseMethodDeclaration (~7782). parsePropertyName falls back
 *     to createMissingNode(Identifier) when the current token cannot begin
 *     a PropertyName, matching the 107_MemberFunctionDeclaration5_es6.ts
 *     fixture (`class C {\n   *\n}`): the `*` method has a zero-width missing
 *     name at the full_start of the following `}` and no parameters / body.
 *
 * Extended alongside selfhost-derived fixtures: parseModifiers (~8015),
 * contextual get/set accessors (~8081), and ConstructorKeyword (~8088),
 * then parsePropertyOrMethodDeclaration (~7835) for the remaining members.
 */
/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parsePropertyDeclaration
 * (~7814) reduced to the subset currently exercised by the parser curriculum:
 *     exclamationToken? TypeAnnotation? (`=` AssignmentExpression)? `;`?
 * The exclamationToken / type slots are consumed but discarded — the oracle
 * walks forEachChildInPropertyDeclaration (~536) which visits modifiers,
 * name, questionToken, exclamationToken, type, initializer. ctsc only
 * models `name` + `initializer` so the JSON children array currently
 * contains at most those two visits (no active fixture needs the others).
 *
 * `parseSemicolonAfterPropertyName` (~2454) is approximated: we consume a
 * trailing `;` when present; ASI / `)` / `}` / line-break handles the rest.
 * finishNode's end = scanner.getTokenFullStart() AFTER the last consumed
 * token, matching cur_full_start(p) at the return point.
 */
static CtscNode* parse_property_declaration_rest(Parser* p, int pos, CtscNode* name,
                                                 const CtscNodeArray* modifiers_opt) {
    /* parseOptionalToken(ExclamationToken) when no QuestionToken was consumed
     * AND scanner.hasPrecedingLineBreak() is false. ctsc does not yet model
     * the `!` definite-assignment modifier — no active fixture demands it —
     * so we only consume it when present. */
    if (cur(p) == CTSC_SK_ExclamationToken
        && !p->scanner.current.has_preceding_line_break) {
        advance(p);
    }
    /* parseTypeAnnotation (~4961): `: Type`. Captured so the JSON emitter
     * can surface it as a PropertyDeclaration child, mirroring
     * forEachChildInPropertyDeclaration (~536) which visits `type` between
     * exclamationToken and initializer. */
    CtscNode* type = parse_type_annotation(p);
    /* parseInitializer: parseOptional(EqualsToken) ? parseAssignmentExpressionOrHigher : undefined. */
    CtscNode* init = NULL;
    if (accept(p, CTSC_SK_EqualsToken)) {
        init = parse_assignment_expression(p);
        if (!init) {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            init = make_missing_identifier(p);
        }
    }
    /* parseSemicolonAfterPropertyName (~2454): consume an explicit `;` when
     * present. ASI / `}` / `)` / line-break terminate the declaration without
     * a token; finishNode.end tracks cur_full_start in that case. */
    if (cur(p) == CTSC_SK_SemicolonToken) advance(p);
    int end = cur_full_start(p);
    CtscNode* pd = ctsc_node_new(p->arena, CTSC_SK_PropertyDeclaration, pos, end);
    if (modifiers_opt) {
        pd->data.propertyDeclaration.modifiers = *modifiers_opt;
    } else {
        ctsc_node_array_init(&pd->data.propertyDeclaration.modifiers);
    }
    pd->data.propertyDeclaration.name = name;
    pd->data.propertyDeclaration.type = type;
    pd->data.propertyDeclaration.initializer = init;
    return pd;
}

static CtscNode* parse_class_element(Parser* p) {
    int pos = cur_full_start(p);

    if (cur(p) == CTSC_SK_SemicolonToken) {
        /* Mirrors upstream parser.ts parseClassElement (~8071):
         *   const pos = getNodePos();
         *   nextToken();
         *   return finishNode(factory.createSemicolonClassElement(), pos);
         * pos = full_start of `;`; end = scanner.getTokenFullStart()
         * after consuming it (== cur_full_start after advance). */
        advance(p); /* `;` */
        int end = cur_full_start(p);
        return ctsc_node_new(p->arena, CTSC_SK_SemicolonClassElement, pos, end);
    }

    /* Mirrors upstream parser.ts parseClassElement (~8076): parseModifiers
     * before dispatching to accessors / constructor / parsePropertyOrMethodDeclaration. */
    CtscNodeArray modifiers; ctsc_node_array_init(&modifiers);
    parse_modifiers(p, &modifiers);

    /* parser.ts ~8081-8087: contextual get / set accessors. */
    if (cur(p) == CTSC_SK_GetKeyword || cur(p) == CTSC_SK_SetKeyword) {
        CtscSyntaxKind accessor_kind = (cur(p) == CTSC_SK_GetKeyword)
            ? CTSC_SK_GetAccessor : CTSC_SK_SetAccessor;
        CtscScanner saved = p->scanner;
        size_t saved_diag_count = p->diagnostics->count;
        advance(p);
        if (token_can_follow_get_or_set(cur(p))) {
            return parse_accessor_declaration(p, pos, accessor_kind, &modifiers);
        }
        p->scanner = saved;
        ctsc_diag_truncate(p->diagnostics, saved_diag_count);
    }

    /* parser.ts ~8088-8094: constructor declaration (ConstructorKeyword). */
    if (cur(p) == CTSC_SK_ConstructorKeyword) {
        CtscNode* name = make_identifier_from_current(p);
        return parse_method_declaration_rest(p, pos,
            false, 0, 0, name, &modifiers);
    }

    /* parsePropertyOrMethodDeclaration (~7835): parseOptionalToken(`*`),
     * parsePropertyName, parseOptionalToken(`?`), then dispatch to
     * parseMethodDeclaration when the next token is `(` / `<` / asteriskToken
     * was present, otherwise parsePropertyDeclaration. */
    CtscSyntaxKind k = cur(p);
    bool is_property_name_start =
        (k == CTSC_SK_AsteriskToken)
        || (k == CTSC_SK_OpenBracketToken)
        || (k == CTSC_SK_StringLiteral)
        || (k == CTSC_SK_NumericLiteral)
        || (k == CTSC_SK_BigIntLiteral)
        || token_is_identifier_or_keyword(k);
    if (!is_property_name_start) return NULL;

    bool has_asterisk = false;
    int asterisk_pos = 0, asterisk_end = 0;
    if (cur(p) == CTSC_SK_AsteriskToken) {
        has_asterisk = true;
        asterisk_pos = cur_full_start(p);
        advance(p); /* `*` */
        asterisk_end = cur_full_start(p);
    }

    CtscNode* name = parse_property_name(p);
    if (cur(p) == CTSC_SK_QuestionToken) advance(p);

    if (has_asterisk || cur(p) == CTSC_SK_OpenParenToken || cur(p) == CTSC_SK_LessThanToken) {
        return parse_method_declaration_rest(p, pos,
            has_asterisk, asterisk_pos, asterisk_end, name, &modifiers);
    }

    return parse_property_declaration_rest(p, pos, name, &modifiers);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * isValidHeritageClauseObjectLiteral (~2945): with the current token being
 * `{`, lookAhead to decide whether the braced block is a valid heritage
 * element (i.e. an empty object literal followed by a token that continues
 * the heritage list) vs. the class body start. The fixture
 * 108_parserErrorRecovery_ExtendsOrImplementsClause1.ts (`class C extends
 * {\n}`) hits the FALSE arm: `{` → `}` → EOF, and EOF is not one of
 * Comma/OpenBrace/Extends/Implements, so `{` is treated as the class body.
 *
 * Callers MUST save/restore `p->scanner` around this helper — it mutates
 * the scanner state to peek ahead.
 */
static bool is_valid_heritage_clause_object_literal(Parser* p) {
    /* Precondition: cur(p) == CTSC_SK_OpenBraceToken. */
    advance(p); /* peek past `{` */
    if (cur(p) != CTSC_SK_CloseBraceToken) {
        /* Non-empty braces: accept as an ObjectLiteralExpression type
         * argument, matching tsc's "return true" fast path. */
        return true;
    }
    advance(p); /* peek past `}` */
    CtscSyntaxKind next = cur(p);
    return next == CTSC_SK_CommaToken
        || next == CTSC_SK_OpenBraceToken
        || next == CTSC_SK_ExtendsKeyword
        || next == CTSC_SK_ImplementsKeyword;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isListElement (~2882)
 * for the HeritageClauseElement parsing context: decide whether the current
 * token can start an ExpressionWithTypeArguments. `{` dispatches to the
 * lookAhead isValidHeritageClauseObjectLiteral guard above; other starters
 * follow isStartOfLeftHandSideExpression with the extends/implements carve-
 * out (which we approximate via token_is_identifier_or_keyword + a few
 * literal/primary-expression starters so the common cases parse).
 */
static bool is_heritage_clause_element_start(Parser* p) {
    CtscSyntaxKind k = cur(p);
    if (k == CTSC_SK_OpenBraceToken) {
        CtscScanner saved = p->scanner;
        bool ok = is_valid_heritage_clause_object_literal(p);
        p->scanner = saved;
        return ok;
    }
    /* isStartOfLeftHandSideExpression + !isHeritageClauseExtendsOrImplementsKeyword.
     * The extends/implements carve-out applies when tsc sees a bare
     * `extends` / `implements` followed by an identifier in a position
     * where the previous clause has already closed — we don't model the
     * disambiguation here because the outer parseList(HeritageClauses)
     * terminator catches those cases by itself. */
    if (k == CTSC_SK_Identifier) return true;
    if (k == CTSC_SK_ThisKeyword || k == CTSC_SK_SuperKeyword
        || k == CTSC_SK_NullKeyword || k == CTSC_SK_TrueKeyword
        || k == CTSC_SK_FalseKeyword) return true;
    if (k == CTSC_SK_NumericLiteral || k == CTSC_SK_BigIntLiteral
        || k == CTSC_SK_StringLiteral
        || k == CTSC_SK_NoSubstitutionTemplateLiteral
        || k == CTSC_SK_TemplateHead) return true;
    if (k == CTSC_SK_OpenParenToken || k == CTSC_SK_OpenBracketToken) return true;
    if (k == CTSC_SK_NewKeyword) return true;
    /* Reserved/contextual keywords that can appear as identifier references
     * (e.g. `extends Promise`, where `Promise` is an Identifier). Reject
     * the two heritage-clause keywords themselves so the outer parseList
     * can close the current clause and start a new one. */
    if (k == CTSC_SK_ExtendsKeyword || k == CTSC_SK_ImplementsKeyword) return false;
    if (token_is_identifier_or_keyword(k)) return true;
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseExpressionWithTypeArguments
 * (~8216):
 *     const expression = parseLeftHandSideExpressionOrHigher();
 *     if (expression.kind === ExpressionWithTypeArguments) return expression;
 *     const typeArguments = tryParseTypeArguments();
 *     return finishNode(factory.createExpressionWithTypeArguments(expression,
 *                                                                 typeArguments), pos);
 *
 * ctsc does not yet pre-coerce PropertyAccess `<TypeArgs>` into an
 * ExpressionWithTypeArguments from parseLeftHandSideExpressionOrHigher, so
 * we always wrap whatever the LHS parser produced. tryParseTypeArguments is
 * a speculative `<...>` parse that bails on mismatch; for the current
 * fixture (heritage types absent) the function is never entered.
 */
static CtscNode* parse_expression_with_type_arguments(Parser* p) {
    int pos = cur_full_start(p);
    CtscNode* expression = parse_left_hand_side_expression_or_higher(p);
    if (!expression) {
        /* Mirrors parser.ts parsePrimaryExpression (~6660) default branch:
         * synthesize a zero-width missing Identifier at the current
         * full_start so parseDelimitedList can make progress. */
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        expression = make_missing_identifier(p);
    }
    CtscNodeArray type_arguments; ctsc_node_array_init(&type_arguments);
    bool has_type_arguments = false;
    if (cur(p) == CTSC_SK_LessThanToken) {
        CtscScanner saved = p->scanner;
        if (try_parse_type_argument_list(p, &type_arguments)) {
            has_type_arguments = true;
        } else {
            p->scanner = saved;
            ctsc_node_array_init(&type_arguments);
        }
    }
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena,
        CTSC_SK_ExpressionWithTypeArguments, pos, end);
    n->data.expressionWithTypeArguments.expression = expression;
    n->data.expressionWithTypeArguments.has_type_arguments = has_type_arguments;
    n->data.expressionWithTypeArguments.type_arguments = type_arguments;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseHeritageClause
 * (~8207):
 *     const pos = getNodePos();
 *     const tok = token();
 *     // isListElement() has already asserted this is ExtendsKeyword /
 *     // ImplementsKeyword.
 *     nextToken();
 *     const types = parseDelimitedList(HeritageClauseElement,
 *                                      parseExpressionWithTypeArguments);
 *     return finishNode(factory.createHeritageClause(tok, types), pos);
 *
 * finishNode's end = scanner.getTokenFullStart() of the token AFTER the
 * last element (or after the clause keyword itself when types is empty).
 * For the 108_parserErrorRecovery_ExtendsOrImplementsClause1.ts fixture
 * that is the full_start of `{` (= 35), because no element passes
 * isValidHeritageClauseObjectLiteral and the scanner hasn't advanced past
 * `{` when the clause closes.
 */
static CtscNode* parse_heritage_clause(Parser* p) {
    int pos = cur_full_start(p);
    CtscSyntaxKind tok = cur(p);
    /* Caller has already verified this via isHeritageClause. */
    advance(p); /* consume `extends` / `implements` */

    CtscNodeArray types; ctsc_node_array_init(&types);
    /* parseDelimitedList(HeritageClauseElement, parseExpressionWithTypeArguments). */
    if (is_heritage_clause_element_start(p)) {
        for (;;) {
            int before = (int)p->scanner.pos;
            CtscNode* e = parse_expression_with_type_arguments(p);
            if (e) ctsc_node_array_push(&types, p->arena, e);
            if ((int)p->scanner.pos == before) break;
            if (!accept(p, CTSC_SK_CommaToken)) break;
            if (!is_heritage_clause_element_start(p)) break;
        }
    }
    int end = cur_full_start(p);
    CtscNode* hc = ctsc_node_new(p->arena, CTSC_SK_HeritageClause, pos, end);
    hc->data.heritageClause.token = tok;
    hc->data.heritageClause.types = types;
    return hc;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseHeritageClauses
 * (~8196):
 *     if (isHeritageClause()) return parseList(HeritageClauses, parseHeritageClause);
 *     return undefined;
 * isHeritageClause is simply `token === ExtendsKeyword ||
 * token === ImplementsKeyword`. parseList's terminator for
 * HeritageClauses is `isListTerminator(HeritageClauses)` which fires on
 * OpenBraceToken (the class body start) — mirrored here by the loop's
 * continue guard.
 */
static void parse_heritage_clauses(Parser* p, CtscNodeArray* out) {
    if (cur(p) != CTSC_SK_ExtendsKeyword && cur(p) != CTSC_SK_ImplementsKeyword) {
        return;
    }
    while (cur(p) == CTSC_SK_ExtendsKeyword || cur(p) == CTSC_SK_ImplementsKeyword) {
        int before = (int)p->scanner.pos;
        CtscNode* hc = parse_heritage_clause(p);
        if (hc) ctsc_node_array_push(out, p->arena, hc);
        if ((int)p->scanner.pos == before) break;
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseClassDeclarationOrExpression
 * (~8154):
 *     parseExpected(ClassKeyword);
 *     const name = parseNameOfClassDeclarationOrExpression();
 *     parseTypeParameters();
 *     parseHeritageClauses();
 *     if (parseExpected(OpenBraceToken)) {
 *         members = parseClassMembers();
 *         parseExpected(CloseBraceToken);
 *     } else {
 *         members = createMissingList();
 *     }
 *     finishNode(kind === ClassDeclaration
 *         ? factory.createClassDeclaration(...)
 *         : factory.createClassExpression(...), pos);
 *
 * finishNode (~2600) records end = scanner.getTokenFullStart() of the token
 * that follows the node. For a well-formed class the final `}` has been
 * consumed, so end = full_start of the next token. For an unterminated
 * class (e.g. the 106_parser512084.ts fixture `class foo {` with EOF inside
 * the body), parseExpected(CloseBraceToken) does not consume anything and
 * finishNode records the current scanner position — which is the full_start
 * of the EOF token (= the offset right after `{`, before the trailing
 * \r\n trivia is absorbed into EOF's leading trivia).
 *
 * `kind` is either CTSC_SK_ClassDeclaration (statement position, via
 * parse_statement) or CTSC_SK_ClassExpression (expression position, via
 * parse_primary). The only difference is the resulting node kind; the
 * header/body grammar is identical — mirrors upstream's shared helper.
 *
 * Class member parsing is delegated to parse_class_element, which currently
 * handles SemicolonClassElement and generator-MethodDeclaration (leading `*`).
 * Unrecognised tokens are brace-balanced and skipped so the outer node's
 * pos/end stays consistent with tsc's finishNode — broader class-member
 * parsing (modifiers, property / method without `*`, accessors, constructors,
 * static blocks) is grown alongside the fixtures that unlock them.
 * parseTypeParameters / parseHeritageClauses are similarly deferred: the
 * fixtures that unlocked these kinds have neither, and the oracle falls
 * through to the default forEachChild branch so only `name` and `members`
 * need to round-trip in JSON.
 */
static CtscNode* parse_class_declaration_or_expression(Parser* p, CtscSyntaxKind kind) {
    /* No-modifiers entry (parse_primary ClassExpression, bare
     * ClassDeclaration without a leading TS modifier). Mirrors upstream
     * parseClassDeclaration ~7517 being called with `modifiersIn = undefined`
     * from the non-modifier case branch in parseStatementWorker (~7406). */
    return parse_class_declaration_or_expression_with_modifiers(p, kind, cur_full_start(p), NULL);
}

/*
 * Worker form used by parse_statement's modifier-prefixed declaration arm.
 * `pos` is the pre-modifier full_start (tsc's getNodePos() captured before
 * parseModifiers in parseDeclaration ~7471). `modifiers` is the NodeArray
 * produced by parseModifiers; pass NULL (or an empty array) when absent.
 * The class header consumption itself is identical to the unmodified path.
 */
static CtscNode* parse_class_declaration_or_expression_with_modifiers(
    Parser* p, CtscSyntaxKind kind, int pos, const CtscNodeArray* modifiers) {
    advance(p); /* `class` */
    CtscNode* name = NULL;
    if (is_binding_identifier_kind(cur(p))) {
        name = make_identifier_from_current(p);
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts
     * parseClassDeclarationOrExpression (~8160):
     *     const typeParameters = parseTypeParameters();
     * Consume `<T, U, ...>` between the (optional) class name and the class
     * body so the `<`…`>` tokens don't leak into the SourceFile statement
     * loop as stray expressions. For `class C<T> {}` this turns the token
     * stream from `class C < T > { }` into `class C { }` by the time we
     * look for OpenBraceToken. Heritage clauses (`extends` / `implements`)
     * are not yet modelled — none of the active fixtures need them. */
    CtscNodeArray type_parameters; ctsc_node_array_init(&type_parameters);
    (void)parse_type_parameters(p, &type_parameters);
    /* Mirrors parser.ts parseClassDeclarationOrExpression (~8162):
     *     const heritageClauses = parseHeritageClauses();
     * Populated when the next token is `extends` or `implements`; stays
     * empty otherwise (tsc's `undefined` NodeArray — the oracle's default
     * forEachChild branch skips empty NodeArrays). */
    CtscNodeArray heritage_clauses; ctsc_node_array_init(&heritage_clauses);
    parse_heritage_clauses(p, &heritage_clauses);
    CtscNodeArray members; ctsc_node_array_init(&members);
    int end;
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        advance(p); /* `{` */
        /* parseClassMembers → parseList(ClassMembers, parseClassElement).
         * The list terminator is `}` or EOF; unrecognised tokens are
         * skipped one at a time (parseList's recovery uses isInSomeParsingContext
         * + nextToken — we mirror the end result without the full scanner
         * reset). */
        while (cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken) {
            int before = (int)p->scanner.pos;
            CtscNode* m = parse_class_element(p);
            if (m) {
                ctsc_node_array_push(&members, p->arena, m);
            }
            if ((int)p->scanner.pos == before) advance(p);
        }
        if (cur(p) == CTSC_SK_CloseBraceToken) {
            advance(p); /* `}` */
            end = cur_full_start(p);
        } else {
            /* Unterminated class body: parseExpected(CloseBraceToken) fails.
             * finishNode end = scanner.getTokenFullStart() which is EOF's
             * full_start (equals the offset just after `{` in the common
             * case of `class foo {` at EOF). */
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
                cur_start(p), cur_end(p) - cur_start(p),
                "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_CloseBraceToken));
            end = cur_full_start(p);
        }
    } else {
        /* parseExpected(OpenBraceToken) fails; members = createMissingList()
         * and finishNode end = cur_full_start. */
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_OpenBraceToken));
        end = cur_full_start(p);
    }
    CtscNode* cd = ctsc_node_new(p->arena, kind, pos, end);
    if (modifiers && modifiers->len > 0) {
        cd->data.classDeclaration.modifiers = *modifiers;
    } else {
        ctsc_node_array_init(&cd->data.classDeclaration.modifiers);
    }
    cd->data.classDeclaration.name = name;
    cd->data.classDeclaration.type_parameters = type_parameters;
    cd->data.classDeclaration.heritage_clauses = heritage_clauses;
    cd->data.classDeclaration.members = members;
    return cd;
}

static CtscNode* parse_class_declaration(Parser* p) {
    return parse_class_declaration_or_expression(p, CTSC_SK_ClassDeclaration);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseInterfaceDeclaration
 * (~8239):
 *     parseExpected(InterfaceKeyword);
 *     const name = parseIdentifier();
 *     const typeParameters = parseTypeParameters();
 *     const heritageClauses = parseHeritageClauses();
 *     const members = parseObjectTypeMembers();
 *     return finishNode(factory.createInterfaceDeclaration(
 *         modifiers, name, typeParameters, heritageClauses, members), pos);
 *
 * parseObjectTypeMembers (~4378) parses "{" TypeMember* "}" and returns a
 * createMissingList when the `{` is missing. ctsc reuses the ClassDeclaration
 * data struct (see ast.h CtscClassDeclarationData) because the JSON shape and
 * forEachChild visit order are identical:
 *     modifiers, name, typeParameters, heritageClauses, members.
 * forEachChildInInterfaceDeclaration (parser.ts ~901) mirrors
 * forEachChildInClassDeclarationOrExpression (~1174) exactly.
 *
 * ctsc does not yet model TypeElement members (PropertySignature /
 * MethodSignature / IndexSignature / ...); the `{...}` body is brace-balanced
 * and skipped so the outer InterfaceDeclaration.end lines up with tsc's
 * finishNode (end = scanner.getTokenFullStart() of the token AFTER `}`). The
 * oracle (harness/src/oracle-ast.ts) has no explicit case for InterfaceDeclaration
 * so it falls through to the default branch, which walks forEachChild and
 * emits a `children` array built from non-empty visits only — name and
 * heritageClauses (when present). For the
 * 108_parserErrorRecovery_ExtendsOrImplementsClause6.ts fixture
 * (`interface I extends { }`) that yields `[Identifier(I), HeritageClause]`,
 * matching the expected output.
 *
 * `name` is always present — parseIdentifier falls through to
 * createMissingNode(Identifier) (parser.ts ~2619) when the current token is
 * a reserved word; ctsc uses make_missing_identifier for parity.
 */
static CtscNode* parse_interface_declaration_with_modifiers(
    Parser* p, int pos, const CtscNodeArray* modifiers) {
    advance(p); /* `interface` */
    /* parseIdentifier: accept real identifiers AND contextual / TS-specific
     * keywords (token > LastReservedWord), mirroring isIdentifier ~2318.
     * When the next token is a reserved word (e.g. `interface void {}`),
     * createMissingNode synthesises a zero-width Identifier at the current
     * full_start; mirror that via make_missing_identifier. */
    CtscNode* name;
    if (is_binding_identifier_kind(cur(p))) {
        name = make_identifier_from_current(p);
    } else {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1003,
            cur_start(p), cur_end(p) - cur_start(p),
            "Identifier expected.");
        name = make_missing_identifier(p);
    }
    CtscNodeArray type_parameters; ctsc_node_array_init(&type_parameters);
    (void)parse_type_parameters(p, &type_parameters);
    CtscNodeArray heritage_clauses; ctsc_node_array_init(&heritage_clauses);
    parse_heritage_clauses(p, &heritage_clauses);
    /* parseObjectTypeMembers (~4378): `{` TypeMember* `}` or createMissingList
     * when parseExpected(OpenBraceToken) fails. ctsc does not yet materialise
     * TypeElement children (no active fixture inspects them); brace-balance
     * the body so finishNode.end matches tsc (full_start of the token AFTER
     * `}`). */
    CtscNodeArray members; ctsc_node_array_init(&members);
    int end;
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        advance(p); /* `{` */
        int depth = 1;
        while (depth > 0 && cur(p) != CTSC_SK_EndOfFileToken) {
            if (cur(p) == CTSC_SK_OpenBraceToken) { depth++; advance(p); continue; }
            if (cur(p) == CTSC_SK_CloseBraceToken) {
                depth--;
                if (depth == 0) break;
                advance(p);
                continue;
            }
            advance(p);
        }
        if (cur(p) == CTSC_SK_CloseBraceToken) {
            advance(p); /* `}` */
            end = cur_full_start(p);
        } else {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
                cur_start(p), cur_end(p) - cur_start(p),
                "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_CloseBraceToken));
            end = cur_full_start(p);
        }
    } else {
        /* parseExpected(OpenBraceToken) fails; members = createMissingList()
         * and finishNode end = scanner.getTokenFullStart(). */
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_OpenBraceToken));
        end = cur_full_start(p);
    }
    CtscNode* id = ctsc_node_new(p->arena, CTSC_SK_InterfaceDeclaration, pos, end);
    /* Share storage with ClassDeclaration: identical shape + forEachChild
     * visit order (modifiers, name, typeParameters, heritageClauses, members). */
    if (modifiers && modifiers->len > 0) {
        id->data.classDeclaration.modifiers = *modifiers;
    } else {
        ctsc_node_array_init(&id->data.classDeclaration.modifiers);
    }
    id->data.classDeclaration.name = name;
    id->data.classDeclaration.type_parameters = type_parameters;
    id->data.classDeclaration.heritage_clauses = heritage_clauses;
    id->data.classDeclaration.members = members;
    return id;
}

/* Entry used by parseStatement when there are no leading modifiers
 * (mirrors upstream parseInterfaceDeclaration called with
 * `modifiersIn = undefined` from the bare-`interface` arm of
 * parseStatementWorker ~7519). */
static CtscNode* parse_interface_declaration(Parser* p) {
    return parse_interface_declaration_with_modifiers(p, cur_full_start(p), NULL);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeAliasDeclaration (~8249).
 * Current token must be TypeKeyword. `decl_start_pos` is the finished node's
 * `pos` (`export` for `export type ...`, otherwise `type`). When `export_mod`
 * is non-NULL it points at a modifier list whose only element is the
 * ExportKeyword spanning the `export` token.
 */
static CtscNode* parse_type_alias_declaration(
    Parser* p, int decl_start_pos, const CtscNodeArray* export_mod) {
    advance(p); /* `type` */
    CtscNode* name;
    if (is_binding_identifier_kind(cur(p))) {
        name = make_identifier_from_current(p);
    } else {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1003,
            cur_start(p), cur_end(p) - cur_start(p),
            "Identifier expected.");
        name = make_missing_identifier(p);
    }
    CtscNodeArray type_parameters;
    ctsc_node_array_init(&type_parameters);
    (void)parse_type_parameters(p, &type_parameters);
    expect(p, CTSC_SK_EqualsToken);
    CtscNode* type = parse_type_in_annotation_position(p, true);
    if (cur(p) == CTSC_SK_SemicolonToken) {
        advance(p);
    }
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_TypeAliasDeclaration, decl_start_pos, end);
    if (export_mod && export_mod->len > 0) {
        n->data.typeAliasDeclaration.modifiers = *export_mod;
    } else {
        ctsc_node_array_init(&n->data.typeAliasDeclaration.modifiers);
    }
    n->data.typeAliasDeclaration.name = name;
    n->data.typeAliasDeclaration.type_parameters = type_parameters;
    n->data.typeAliasDeclaration.type = type;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseEnumDeclaration (~8275):
 *     parseExpected(EnumKeyword);
 *     const name = parseIdentifier();
 *     if (parseExpected(OpenBraceToken)) {
 *         members = doOutsideOfYieldAndAwaitContext(
 *             () => parseDelimitedList(ParsingContext.EnumMembers, parseEnumMember));
 *         parseExpected(CloseBraceToken);
 *     } else {
 *         members = createMissingList<EnumMember>();
 *     }
 *     return finishNode(factory.createEnumDeclaration(modifiers, name, members), pos);
 *
 * parseIdentifier (~2688) falls through createIdentifier's `isIdentifier`
 * branch when the current token is a reserved word: createMissingNode
 * (~2619) produces a zero-width Identifier at scanner.getTokenFullStart()
 * with an empty escapedText. That is exactly what the
 * 106_parserEnumDeclaration4.ts fixture (`enum void {}`) requires: the
 * missing Identifier sits at full_start of `void` (= 24 in that fixture's
 * byte layout with CRLF line endings), and EnumDeclaration.end equals that
 * same full_start because parseExpected(OpenBraceToken) also fails without
 * consuming anything (the current token is still `void`), so finishNode's
 * `end = scanner.getTokenFullStart()` is still full_start of `void`.
 *
 * ctsc does not yet model modifiers or EnumMember parsing; both are grown
 * alongside fixtures that exercise them. parseEnumMember itself is left as
 * a stub here — the current fixture has no members. When a member-producing
 * fixture unlocks, parse_enum_member will parse PropertyName + optional
 * `=` Initializer and finishNode with factory.createEnumMember.
 */
static CtscNode* parse_enum_member(Parser* p) {
    int fs = cur_full_start(p);
    CtscNode* name = parse_property_name(p);
    CtscNode* init = NULL;
    if (accept(p, CTSC_SK_EqualsToken)) {
        init = parse_assignment_expression(p);
        if (!init) {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            init = make_missing_identifier(p);
        }
    }
    /* EnumMember shares PropertyAssignment's shape (name + optional
     * initializer) and forEachChild visit order, but tsc emits it with the
     * SyntaxKind.EnumMember kind so the oracle prints `"kind":"EnumMember"`.
     * Reuse the propertyAssignment union member for storage; ast_json.c
     * routes the EnumMember case to the same serialisation path. */
    int end = init ? init->end : name->end;
    CtscNode* em = ctsc_node_new(p->arena, CTSC_SK_EnumMember, fs, end);
    em->data.propertyAssignment.name = name;
    em->data.propertyAssignment.initializer = init;
    return em;
}

static CtscNode* parse_enum_declaration(Parser* p) {
    int pos = cur_full_start(p);
    advance(p); /* `enum` */
    /* parseIdentifier: when the current token is a reserved word (e.g.
     * `void` from the 106_parserEnumDeclaration4.ts fixture), createIdentifier's
     * `isIdentifier` branch is false and createMissingNode(Identifier) (~2619)
     * yields a zero-width Identifier at scanner.getTokenFullStart() without
     * consuming anything. When it IS an identifier (or a contextual / TS
     * keyword accepted as one — see is_binding_identifier_kind), consume it. */
    CtscNode* name;
    if (is_binding_identifier_kind(cur(p))) {
        name = make_identifier_from_current(p);
    } else {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1003,
            cur_start(p), cur_end(p) - cur_start(p),
            "Identifier expected.");
        name = make_missing_identifier(p);
    }
    CtscNodeArray members; ctsc_node_array_init(&members);
    int end;
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        advance(p); /* `{` */
        while (cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken) {
            int before = (int)p->scanner.pos;
            CtscNode* m = parse_enum_member(p);
            if (m) ctsc_node_array_push(&members, p->arena, m);
            if ((int)p->scanner.pos == before) advance(p);
            if (!accept(p, CTSC_SK_CommaToken)) break;
        }
        if (cur(p) == CTSC_SK_CloseBraceToken) {
            advance(p); /* `}` */
            end = cur_full_start(p);
        } else {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
                cur_start(p), cur_end(p) - cur_start(p),
                "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_CloseBraceToken));
            end = cur_full_start(p);
        }
    } else {
        /* parseExpected(OpenBraceToken) fails; members = createMissingList()
         * and finishNode end = scanner.getTokenFullStart() — which equals the
         * full_start of the current token (unchanged by parseIdentifier's
         * missing-node synthesis). */
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_OpenBraceToken));
        end = cur_full_start(p);
    }
    CtscNode* ed = ctsc_node_new(p->arena, CTSC_SK_EnumDeclaration, pos, end);
    ed->data.enumDeclaration.name = name;
    ed->data.enumDeclaration.members = members;
    return ed;
}

static CtscNode* parse_block(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_OpenBraceToken);
    /* Mirrors upstream parser.ts parseBlock (~6830):
     *     const multiLine = scanner.hasPrecedingLineBreak();
     * captured on the first token AFTER the opening brace. A line break
     * between `{` and the first body token (or the closing `}` for an
     * empty block) sets multiLine=true; the printer then emits the body
     * on its own line(s). */
    bool multi_line = p->scanner.current.has_preceding_line_break;
    CtscNodeArray stmts; ctsc_node_array_init(&stmts);
    while (cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken) {
        int before = (int)p->scanner.pos;
        CtscNode* s = parse_statement(p);
        if (s) ctsc_node_array_push(&stmts, p->arena, s);
        if ((int)p->scanner.pos == before) advance(p);
    }
    int end = cur_end(p);
    expect(p, CTSC_SK_CloseBraceToken);
    CtscNode* b = ctsc_node_new(p->arena, CTSC_SK_Block, fs, end);
    b->data.block.statements = stmts;
    b->data.block.multi_line = multi_line;
    return b;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseBlock(ignoreMissingOpenBrace=false)
 * (~6824). When the current token is `{`, delegate to parse_block (which
 * matches the `openBraceParsed` branch). Otherwise reproduce the missing-
 * brace branch (~6841): emit the "'{' expected." diagnostic without
 * advancing the scanner, and finishNode a zero-width Block at the current
 * token full_start with an empty statements list.
 *
 * Used by parse_try_statement so that a bare `finally` (the
 * 106_parserMissingToken1.ts fixture: `a / finally` at EOF) synthesises
 * empty tryBlock and finallyBlock nodes at the exact positions tsc records
 * (pos = full_start of the current token, end = pos since the scanner is
 * not advanced). Call sites inside parse_block itself are left on the
 * legacy semantics (consume tokens until matching `}`) because they enter
 * with the `{` already under the cursor.
 */
static CtscNode* parse_block_or_missing(Parser* p) {
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        return parse_block(p);
    }
    int fs = cur_full_start(p);
    ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
        cur_start(p), cur_end(p) - cur_start(p),
        "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_OpenBraceToken));
    CtscNode* b = ctsc_node_new(p->arena, CTSC_SK_Block, fs, fs);
    ctsc_node_array_init(&b->data.block.statements);
    return b;
}

static CtscNode* parse_statement(Parser* p);

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseModuleBlock (~8290):
 *     ModuleBlock:
 *       "{" parseList(BlockStatements, parseStatement) "}"
 *     else createMissingList<Statement>()
 * finishNode positions: pos = full_start of `{` (captured at entry via
 * getNodePos), end = scanner.getTokenFullStart() after consuming `}`, i.e.
 * cur_full_start(p) right after accept(CloseBraceToken). When `{` is
 * missing we emit a zero-width ModuleBlock at the current full_start,
 * matching tsc's finishNode-after-parseExpected-failure semantics (the
 * scanner is not advanced, so getTokenFullStart stays put).
 */
static CtscNode* parse_module_block(Parser* p) {
    int fs = cur_full_start(p);
    CtscNodeArray stmts; ctsc_node_array_init(&stmts);
    int end;
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        advance(p); /* `{` */
        while (cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken) {
            int before = (int)p->scanner.pos;
            CtscNode* s = parse_statement(p);
            if (s) ctsc_node_array_push(&stmts, p->arena, s);
            if ((int)p->scanner.pos == before) advance(p);
        }
        if (cur(p) == CTSC_SK_CloseBraceToken) {
            advance(p); /* `}` */
            end = cur_full_start(p);
        } else {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
                cur_start(p), cur_end(p) - cur_start(p),
                "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_CloseBraceToken));
            end = cur_full_start(p);
        }
    } else {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_OpenBraceToken));
        end = fs;
    }
    CtscNode* mb = ctsc_node_new(p->arena, CTSC_SK_ModuleBlock, fs, end);
    mb->data.moduleBlock.statements = stmts;
    return mb;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * parseModuleOrNamespaceDeclaration (~8303) / parseModuleDeclaration (~8338).
 * For the only currently-unlocked shape (`namespace foo {}` /
 * `module foo {}` with a dotted or simple identifier and a block body):
 *     parseExpected(NamespaceKeyword | ModuleKeyword);
 *     const name = parseIdentifier();            // or parseIdentifierName
 *                                                // for nested namespaces
 *     const body = parseOptional(DotToken)
 *         ? parseModuleOrNamespaceDeclaration(...) // nested
 *         : parseModuleBlock();
 *     return finishNode(factory.createModuleDeclaration(...), pos);
 *
 * The ambient-external-module form (`module "foo" { }` / `declare module
 * "foo";`) and the global-augmentation form (`global { }`) are not yet
 * modelled — they are gated on StringLiteral / GlobalKeyword which no
 * currently-unlocked fixture exercises.
 *
 * `pos` is captured by the caller (parse_statement) so the outer
 * ModuleDeclaration.pos includes the `namespace`/`module` keyword's leading
 * trivia, matching tsc's getNodePos() captured before parseExpected.
 */
static CtscNode* parse_module_declaration_rest(Parser* p, int pos, bool nested) {
    (void)nested; /* ctsc does not yet distinguish parseIdentifierName vs
                   * parseIdentifier for nested namespaces — both accept
                   * contextual / TS keywords as identifiers, which is the
                   * only subset any currently-unlocked fixture requires. */
    CtscNode* name;
    if (is_binding_identifier_kind(cur(p))) {
        name = make_identifier_from_current(p);
    } else {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1003,
            cur_start(p), cur_end(p) - cur_start(p),
            "Identifier expected.");
        name = make_missing_identifier(p);
    }
    CtscNode* body;
    if (accept(p, CTSC_SK_DotToken)) {
        int inner_pos = cur_full_start(p);
        body = parse_module_declaration_rest(p, inner_pos, /*nested*/ true);
    } else {
        body = parse_module_block(p);
    }
    int end = body ? body->end : (name ? name->end : pos);
    CtscNode* md = ctsc_node_new(p->arena, CTSC_SK_ModuleDeclaration, pos, end);
    md->data.moduleDeclaration.name = name;
    md->data.moduleDeclaration.body = body;
    return md;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * parseAmbientExternalModuleDeclaration (~8315). The ambient-external-module
 * form (`module "foo" { }` / `module "foo";`) uses a StringLiteral as its
 * name. `module "foo" { }` (the shape 107_parserModuleDeclaration1.ts
 * exercises) builds `children:[StringLiteral, ModuleBlock]`; the bodiless
 * `declare module "foo";` form produces `children:[StringLiteral]` alone
 * (body stays NULL, and parseSemicolon consumes an explicit `;`). The
 * GlobalKeyword global-augmentation branch (name = Identifier "global") is
 * not exercised by any currently-unlocked fixture; when one unlocks, add
 * the `if (cur == GlobalKeyword) { name = make_identifier_from_current; }`
 * branch here mirroring parser.ts ~8318.
 */
static CtscNode* parse_ambient_external_module_declaration(Parser* p, int pos) {
    int name_fs = cur_full_start(p);
    CtscNode* name = ctsc_node_new(p->arena, CTSC_SK_StringLiteral, name_fs, cur_end(p));
    name->data.stringLiteral.text = p->scanner.current.text;
    name->data.stringLiteral.text_len = p->scanner.current.text_len;
    name->data.stringLiteral.value = p->scanner.current.value;
    name->data.stringLiteral.value_len = p->scanner.current.value_len;
    name->data.stringLiteral.single_quote =
        (p->scanner.current.text_len > 0 && p->scanner.current.text[0] == '\'');
    advance(p); /* consume the StringLiteral */

    CtscNode* body = NULL;
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        body = parse_module_block(p);
    } else {
        /* parseSemicolon (~2573): accept explicit `;` or ASI at `}` / EOF /
         * after a line break. Emit "';' expected." on a hard miss. */
        if (cur(p) == CTSC_SK_SemicolonToken) {
            advance(p);
        } else if (!can_parse_semicolon_here(p)) {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
                cur_start(p), cur_end(p) - cur_start(p),
                "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_SemicolonToken));
        }
    }
    int end = body ? body->end : cur_full_start(p);
    CtscNode* md = ctsc_node_new(p->arena, CTSC_SK_ModuleDeclaration, pos, end);
    md->data.moduleDeclaration.name = name;
    md->data.moduleDeclaration.body = body;
    return md;
}

static CtscNode* parse_module_declaration(Parser* p) {
    int pos = cur_full_start(p);
    /* Mirrors parser.ts parseModuleDeclaration (~8338): the NamespaceKeyword
     * branch uses parseOptional; when the keyword is `module` and the next
     * token is a StringLiteral we take the ambient-external-module path
     * (~8349). The GlobalKeyword branch (~8340) is not yet modelled — no
     * currently-unlocked fixture exercises global augmentation. */
    bool is_module = (cur(p) == CTSC_SK_ModuleKeyword);
    advance(p); /* consume `module` or `namespace` */
    if (is_module && cur(p) == CTSC_SK_StringLiteral) {
        return parse_ambient_external_module_declaration(p, pos);
    }
    return parse_module_declaration_rest(p, pos, /*nested*/ false);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTryStatement (~7078):
 *     parseExpected(TryKeyword);
 *     const tryBlock = parseBlock(false);
 *     const catchClause = token() === CatchKeyword ? parseCatchClause() : undefined;
 *     // If no catch clause we must have a finally clause; also consume one
 *     // when the current token is FinallyKeyword (covers `try {} catch {} finally {}`).
 *     let finallyBlock;
 *     if (!catchClause || token() === FinallyKeyword) {
 *         parseExpected(FinallyKeyword, Diagnostics.catch_or_finally_expected);
 *         finallyBlock = parseBlock(false);
 *     }
 *     return finishNode(factory.createTryStatement(tryBlock, catchClause, finallyBlock), pos);
 *
 * parseStatement (~7427-7432) routes TryKeyword, CatchKeyword, and
 * FinallyKeyword here — the latter two are error-recovery entries so
 * `try`-less fragments still parse as a TryStatement with a zero-width
 * missing tryBlock (see parse_block_or_missing). The
 * 106_parserMissingToken1.ts fixture (`a / finally` at EOF) hits exactly
 * that path: TryStatement.pos = full_start(`finally`) = 22,
 * tryBlock = Block[22,22], finallyBlock = Block[30,30] after consuming
 * `finally`; TryStatement.end = 30 (finishNode = scanner.getTokenFullStart()
 * at return time = EOF full_start).
 *
 * parseCatchClause (~7097) populates tryStatement.catchClause when the token
 * after the try block is `catch`.
 */
static CtscNode* parse_catch_clause(Parser* p) {
    int pos = cur_full_start(p);
    expect(p, CTSC_SK_CatchKeyword);
    CtscNode* variableDeclaration = NULL;
    if (accept(p, CTSC_SK_OpenParenToken)) {
        variableDeclaration = parse_variable_declaration(p);
        expect(p, CTSC_SK_CloseParenToken);
    }
    CtscNode* block = parse_block_or_missing(p);
    int end = block ? block->end : cur_full_start(p);
    CtscNode* cc = ctsc_node_new(p->arena, CTSC_SK_CatchClause, pos, end);
    cc->data.catchClause.variableDeclaration = variableDeclaration;
    cc->data.catchClause.block = block;
    return cc;
}

static CtscNode* parse_try_statement(Parser* p) {
    int pos = cur_full_start(p);
    expect(p, CTSC_SK_TryKeyword);
    CtscNode* tryBlock = parse_block_or_missing(p);
    CtscNode* catchClause = NULL;
    if (cur(p) == CTSC_SK_CatchKeyword) {
        catchClause = parse_catch_clause(p);
    }
    CtscNode* finallyBlock = NULL;
    if (!catchClause || cur(p) == CTSC_SK_FinallyKeyword) {
        /* parseExpected(FinallyKeyword, Diagnostics.catch_or_finally_expected). */
        if (cur(p) == CTSC_SK_FinallyKeyword) {
            advance(p);
        } else {
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1472,
                cur_start(p), cur_end(p) - cur_start(p),
                "'catch' or 'finally' expected.");
        }
        finallyBlock = parse_block_or_missing(p);
    }
    /* finishNode end = scanner.getTokenFullStart() at return time. When the
     * finally branch ran, finallyBlock->end already equals that value (its
     * own finishNode records the same scanner position). Otherwise the end
     * follows tryBlock (for the not-yet-modelled `try {} catch {}` shape). */
    int end = finallyBlock ? finallyBlock->end
            : (catchClause ? catchClause->end : tryBlock->end);
    CtscNode* ts = ctsc_node_new(p->arena, CTSC_SK_TryStatement, pos, end);
    ts->data.tryStatement.tryBlock = tryBlock;
    ts->data.tryStatement.catchClause = catchClause;
    ts->data.tryStatement.finallyBlock = finallyBlock;
    return ts;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectBindingElement
 * (~7594):
 *     const pos = getNodePos();
 *     const dotDotDotToken = parseOptionalToken(DotDotDotToken);
 *     const tokenIsIdentifier = isBindingIdentifier();
 *     let propertyName: PropertyName | undefined = parsePropertyName();
 *     let name: BindingName;
 *     if (tokenIsIdentifier && token() !== ColonToken) {
 *         name = propertyName as Identifier; propertyName = undefined;
 *     } else {
 *         parseExpected(ColonToken);
 *         name = parseIdentifierOrPattern();
 *     }
 *     const initializer = parseInitializer();
 *     return finishNode(factory.createBindingElement(dotDotDotToken,
 *         propertyName, name, initializer), pos);
 *
 * For the shorthand `{ a }` form `parsePropertyName` consumes the Identifier
 * and — because the next token is `,` / `}` (not `:`) — it is re-seated as
 * the BindingElement's `name`, matching 108_parserForOfStatement13.ts's
 * expected shape where each BindingElement has a single Identifier child.
 */
static CtscNode* parse_object_binding_element(Parser* p) {
    int fs = cur_full_start(p);
    bool has_ddd = false; int ddd_pos = 0, ddd_end = 0;
    if (cur(p) == CTSC_SK_DotDotDotToken) {
        has_ddd = true;
        ddd_pos = cur_full_start(p);
        ddd_end = cur_end(p);
        advance(p);
    }
    bool token_is_identifier = is_binding_identifier_kind(cur(p));
    CtscNode* propertyName = parse_property_name(p);
    CtscNode* name = NULL;
    if (token_is_identifier && cur(p) != CTSC_SK_ColonToken) {
        name = propertyName;
        propertyName = NULL;
    } else {
        expect(p, CTSC_SK_ColonToken);
        name = parse_identifier_or_pattern(p);
    }
    /* parseInitializer (~5065): parseOptional(EqualsToken) ?
     *     parseAssignmentExpressionOrHigher : undefined. */
    CtscNode* init = NULL;
    if (accept(p, CTSC_SK_EqualsToken)) {
        init = parse_assignment_expression(p);
    }
    /* finishNode end = scanner.getTokenFullStart() after the last sub-parse. */
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_BindingElement, fs, end);
    n->data.bindingElement.propertyName = propertyName;
    n->data.bindingElement.name = name;
    n->data.bindingElement.initializer = init;
    n->data.bindingElement.has_dotdotdot = has_ddd;
    n->data.bindingElement.dotdotdot_pos = ddd_pos;
    n->data.bindingElement.dotdotdot_end = ddd_end;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseArrayBindingElement
 * (~7583):
 *     if (token() === CommaToken) return finishNode(createOmittedExpression(), pos);
 *     const dotDotDotToken = parseOptionalToken(DotDotDotToken);
 *     const name = parseIdentifierOrPattern();
 *     const initializer = parseInitializer();
 *     return finishNode(createBindingElement(dotDotDotToken, undefined, name, initializer), pos);
 */
static CtscNode* parse_array_binding_element(Parser* p) {
    int fs = cur_full_start(p);
    if (cur(p) == CTSC_SK_CommaToken) {
        return ctsc_node_new(p->arena, CTSC_SK_OmittedExpression, fs, fs);
    }
    bool has_ddd = false; int ddd_pos = 0, ddd_end = 0;
    if (cur(p) == CTSC_SK_DotDotDotToken) {
        has_ddd = true;
        ddd_pos = cur_full_start(p);
        ddd_end = cur_end(p);
        advance(p);
    }
    CtscNode* name = parse_identifier_or_pattern(p);
    CtscNode* init = NULL;
    if (accept(p, CTSC_SK_EqualsToken)) {
        init = parse_assignment_expression(p);
    }
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_BindingElement, fs, end);
    n->data.bindingElement.propertyName = NULL;
    n->data.bindingElement.name = name;
    n->data.bindingElement.initializer = init;
    n->data.bindingElement.has_dotdotdot = has_ddd;
    n->data.bindingElement.dotdotdot_pos = ddd_pos;
    n->data.bindingElement.dotdotdot_end = ddd_end;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isObjectBindingElement
 * start (parseDelimitedList(ObjectBindingElements).isListElement ~3447).
 * ObjectBindingElement starts with `...`, a literal property name (identifier /
 * keyword / string / number), or `[` (ComputedPropertyName).
 */
static bool is_object_binding_element_start(CtscSyntaxKind k) {
    if (k == CTSC_SK_DotDotDotToken) return true;
    if (k == CTSC_SK_OpenBracketToken) return true; /* ComputedPropertyName */
    if (k == CTSC_SK_StringLiteral) return true;
    if (k == CTSC_SK_NumericLiteral) return true;
    if (k == CTSC_SK_Identifier) return true;
    if (k >= CTSC_SK_BreakKeyword && k <= CTSC_SK_UnknownKeyword) return true;
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectBindingPattern
 * (~7612):
 *     parseExpected(OpenBraceToken);
 *     const elements = allowInAnd(() =>
 *         parseDelimitedList(ObjectBindingElements, parseObjectBindingElement));
 *     parseExpected(CloseBraceToken);
 * finishNode end = scanner.getTokenFullStart() of the token AFTER `}`
 * (== cur_full_start(p) right after consuming `}`). When `}` is missing
 * (e.g. EOF immediately after `{`), parseExpected(CloseBrace) fails without
 * advancing, leaving end at the full_start of the offending token — that
 * preserves the 107_parserErrorRecovery_ParameterList5.ts shape (empty
 * pattern at EOF with end = EOF.full_start = 35).
 */
static CtscNode* parse_object_binding_pattern(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_OpenBraceToken);
    CtscNodeArray elements; ctsc_node_array_init(&elements);
    while (cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken) {
        int before = (int)p->scanner.pos;
        if (is_object_binding_element_start(cur(p))) {
            CtscNode* e = parse_object_binding_element(p);
            if (e) ctsc_node_array_push(&elements, p->arena, e);
            if (accept(p, CTSC_SK_CommaToken)) continue;
            /* No comma: either `}` / EOF terminates the list (handled by the
             * outer loop), or the element parser failed to advance. */
            if ((int)p->scanner.pos == before) advance(p);
            continue;
        }
        /* Stray token inside `{ ... }`: skip it so the brace balance is
         * preserved, matching tsc's parseDelimitedList recovery which
         * advances past an unrecognised token (parser.ts ~3532). */
        advance(p);
        if ((int)p->scanner.pos == before) break;
    }
    expect(p, CTSC_SK_CloseBraceToken);
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_ObjectBindingPattern, fs, end);
    n->data.bindingPattern.elements = elements;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseArrayBindingPattern
 * (~7620): `[` parseDelimitedList(ArrayBindingElements, parseArrayBindingElement) `]`.
 * The element parser handles the `,` → OmittedExpression hole case itself, so
 * the loop here just walks element-comma pairs until `]` / EOF.
 */
static CtscNode* parse_array_binding_pattern(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_OpenBracketToken);
    CtscNodeArray elements; ctsc_node_array_init(&elements);
    while (cur(p) != CTSC_SK_CloseBracketToken && cur(p) != CTSC_SK_EndOfFileToken) {
        int before = (int)p->scanner.pos;
        CtscNode* e = parse_array_binding_element(p);
        if (e) ctsc_node_array_push(&elements, p->arena, e);
        if (accept(p, CTSC_SK_CommaToken)) continue;
        if ((int)p->scanner.pos == before) advance(p);
    }
    expect(p, CTSC_SK_CloseBracketToken);
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_ArrayBindingPattern, fs, end);
    n->data.bindingPattern.elements = elements;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseIdentifierOrPattern
 * (~4850): dispatch on the current token between `{` → ObjectBindingPattern,
 * `[` → ArrayBindingPattern, otherwise BindingIdentifier. Used by both
 * parseNameOfParameter (~4001) and parseVariableDeclaration name parsing.
 */
static CtscNode* parse_identifier_or_pattern(Parser* p) {
    if (cur(p) == CTSC_SK_OpenBraceToken) return parse_object_binding_pattern(p);
    if (cur(p) == CTSC_SK_OpenBracketToken) return parse_array_binding_pattern(p);
    /* Mirrors parseIdentifierOrPattern (~7635): the non-pattern branch is
     * parseBindingIdentifier, not parseIdentifier — contextual / TS-specific
     * keywords are valid binding names (e.g. `var of`). */
    return parse_binding_identifier(p);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseParameterWorker
 * (~4036). ctsc subset:
 *   - optional leading `...` (rest) consumed via parseOptionalToken
 *     (DotDotDotToken) at parser.ts:4069. We record the token span so the
 *     emitter can reproduce `...name` in the JS output (the TS→JS transform
 *     preserves rest parameters verbatim — transformers/ts.ts
 *     visitParameter ~2069).
 *   - name = BindingIdentifier | ObjectBindingPattern | ArrayBindingPattern.
 *   - optional `?` (questionToken) after the name at parser.ts:4081. This is
 *     a TS-only marker; transformers/ts.ts visitParameter drops it before
 *     emit. We advance past it (so the parameter list loop can see the
 *     next comma / `)`) but do not surface it to the emitter, matching the
 *     107_parserParameterList11.ts fixture expectation (`(...arg?) => 102`
 *     → `(...arg) => 102;`).
 *   - optional `: Type` annotation consumed via parse_type_annotation so the
 *     Parameter.end extends through it (Parameter end = finishNode's end =
 *     scanner.getTokenFullStart() after the last consumed sub-node).
 *   - optional `= Initializer` via parseInitializer (~5065): parseOptional
 *     (EqualsToken) + parseAssignmentExpressionOrHigher — mirrors parser.ts
 *     parseParameterWorker (~4077-4083). Needed for class/function default
 *     parameters (emitter/selfhost-derived/85_class_default_params.ts).
 */
/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseModifiers for the
 * subset valid on parameter properties (utilitiesPublic.ts
 * ModifierFlags.ParameterPropertyModifier).
 */
static bool is_parameter_property_modifier_start(CtscSyntaxKind k) {
    return k == CTSC_SK_PublicKeyword
        || k == CTSC_SK_PrivateKeyword
        || k == CTSC_SK_ProtectedKeyword
        || k == CTSC_SK_ReadonlyKeyword;
}

static CtscNode* parse_parameter(Parser* p) {
    int fs = cur_full_start(p);
    bool is_parameter_property = false;
    while (is_parameter_property_modifier_start(cur(p))) {
        is_parameter_property = true;
        advance(p);
    }
    bool has_ddd = false;
    int ddd_pos = 0, ddd_end = 0;
    if (cur(p) == CTSC_SK_DotDotDotToken) {
        has_ddd = true;
        ddd_pos = cur_full_start(p);
        ddd_end = cur_end(p);
        advance(p);
    }
    CtscNode* name = parse_identifier_or_pattern(p);
    if (!name) return NULL;
    /* parseOptionalToken(QuestionToken) at parser.ts:4081 — consumed to
     * advance the scanner. Its span is not stored: the JS emitter drops it
     * (TS-only) and no AST-JSON oracle fixture currently inspects a
     * Parameter's questionToken child. */
    int q_end = -1;
    if (cur(p) == CTSC_SK_QuestionToken) {
        q_end = cur_end(p);
        advance(p);
    }
    /* parseTypeAnnotation (~4961): `: Type`. parse_type_annotation already
     * guards on ColonToken and returns NULL when absent. Its finishNode end
     * equals scanner.getTokenFullStart() after the type is consumed, which
     * is what Parameter.end should propagate to (mirrors tsc's finishNode
     * running after every sub-parse). */
    CtscNode* type = parse_type_annotation(p);
    int end = type ? type->end : (q_end >= 0 ? q_end : name->end);
    CtscNode* initializer = NULL;
    if (accept(p, CTSC_SK_EqualsToken)) {
        /* Mirrors upstream parser.ts parseInitializer (~5065). */
        initializer = parse_assignment_expression(p);
        if (initializer) end = initializer->end;
    }
    CtscNode* pm = ctsc_node_new(p->arena, CTSC_SK_Parameter, fs, end);
    pm->data.parameter.name = name;
    pm->data.parameter.type = type;
    pm->data.parameter.initializer = initializer;
    pm->data.parameter.has_dot_dot_dot = has_ddd;
    pm->data.parameter.dot_dot_dot_pos = ddd_pos;
    pm->data.parameter.dot_dot_dot_end = ddd_end;
    pm->data.parameter.is_parameter_property = is_parameter_property;
    return pm;
}

/*
 * Shared parameter-list + return-type + body parser used by both
 * parse_function_declaration (parser.ts parseFunctionDeclaration ~7734)
 * and parse_function_expression (parser.ts parseFunctionExpression ~6765).
 * Mirrors upstream's parseParameters (~4150) + parseReturnType (~4961) +
 * parseFunctionBlockOrSemicolon (~7567) pipeline. Populates *out_params
 * with the parsed ParameterDeclaration NodeArray, *out_body with the
 * parsed Block (or NULL for an overload-signature / semicolon-only shape),
 * and *out_body_end with the end position finishNode should record for
 * the enclosing FunctionDeclaration / FunctionExpression.
 */
static void parse_function_signature_and_body(Parser* p,
                                              CtscNodeArray* out_params,
                                              CtscNode** out_body,
                                              int* out_body_end,
                                              bool is_async_function) {
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseParameters
     * (~4150): when parseExpected(OpenParen) fails the function bails out
     * via `return createMissingList<ParameterDeclaration>()` WITHOUT entering
     * parseDelimitedList, so the scanner is left parked at the offending
     * token. Only the success path runs parseDelimitedList(Parameters, ...),
     * which is where tsc's error-recovery (advance past stray tokens that
     * outer contexts don't claim) lives. The 106_parserEqualsGreaterThan-
     * AfterFunction1.ts fixture (`function =>`) relies on THIS bail-out: the
     * scanner stays at `=>` so parseFunctionBlockOrSemicolon falls through
     * to parseBlock which synthesises a zero-width Block body. */
    bool open_paren_parsed = accept(p, CTSC_SK_OpenParenToken);
    if (!open_paren_parsed) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005,
            cur_start(p), cur_end(p) - cur_start(p),
            "'%s' expected.", ctsc_syntax_kind_name(CTSC_SK_OpenParenToken));
    }
    CtscNodeArray params; ctsc_node_array_init(&params);
    if (open_paren_parsed) {
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseDelimitedList
         * (~3489) for ParsingContext.Parameters, including the error-recovery
         * branches: `isListElement` (~2913 Parameters → isStartOfParameter),
         * `isListTerminator` (~3034: `)` / `]` / EOF), and
         * `abortParsingListOrMoveToNextToken` (~3410) which advances past a
         * stray token UNLESS an outer parsing context (SourceElements here)
         * claims it.
         *
         * The 107_parserEqualsGreaterThanAfterFunction2.ts fixture
         * (`function (a => b;`) drives this shape: after parameter `a`, the
         * current token is `=>` which is not a Comma, not a CloseParen, not
         * EOF, and is not a statement start — so we skip past it, find `b` as
         * the next parameter, then after `b` we see `;` which again is not a
         * statement start (SemicolonToken is treated as empty statement only
         * when NOT in error recovery; see isListElement(SourceElements,
         * inErrorRecovery=true) at parser.ts ~2851). Skip past `;` as well,
         * then hit EOF which terminates the parameter list. The close-paren
         * `expect` then emits `')' expected.` at EOF, and
         * parseFunctionBlockOrSemicolon's canParseSemicolon() branch leaves
         * `body` undefined, matching tsc's overload-signature shape that the
         * TS→JS transformer drops. */
        for (;;) {
            if (cur(p) == CTSC_SK_CloseParenToken) break;
            if (cur(p) == CTSC_SK_EndOfFileToken) break;
            /* Mirrors isStartOfParameter (~3993): rest (`...`) and binding
             * patterns `{`/`[` — same as parse_parenthesized_arrow_function. */
            if (is_start_of_parameter(p)) {
                int el_fs = cur_full_start(p);
                CtscNode* pm = parse_parameter(p);
                if (!pm) break;
                ctsc_node_array_push(&params, p->arena, pm);
                if (accept(p, CTSC_SK_CommaToken)) continue;
                if (cur(p) == CTSC_SK_CloseParenToken) break;
                if (cur(p) == CTSC_SK_EndOfFileToken) break;
                /* parseExpected(CommaToken) emits "',' expected." without
                 * advancing. The zero-progress guard (parser.ts ~3532)
                 * advances the scanner when parse_parameter consumed no
                 * tokens — cannot happen here since parse_identifier
                 * advances — but kept defensively. */
                if (el_fs == cur_full_start(p)) advance(p);
                continue;
            }
            /* Token is not a parameter start, close-paren, or EOF. Let an
             * outer SourceElements context reclaim it if it looks like a
             * statement start; otherwise advance past it to resynchronise.
             * Mirrors abortParsingListOrMoveToNextToken ~3410 +
             * isInSomeParsingContext ~3078 with the SourceElements branch of
             * isListElement (inErrorRecovery=true, which rejects
             * SemicolonToken — see ~2851 — so a bare `;` inside a broken
             * parameter list is skipped rather than treated as a statement
             * boundary). */
            if (is_start_of_statement_token(p)
                && cur(p) != CTSC_SK_SemicolonToken) break;
            advance(p);
        }
        expect(p, CTSC_SK_CloseParenToken);
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts
     * parseFunctionDeclaration (~7746):
     *     const type = parseReturnType(SyntaxKind.ColonToken, isType=false);
     * The oracle (harness/src/oracle-ast.ts) does not emit a `type` field for
     * FunctionDeclaration, so we consume the annotation purely to advance the
     * scanner past it — matching tsc's body position. Without this, a return
     * type like `: {}` is left unconsumed and the following `{` body is
     * misparsed (the `{}` is picked up as a top-level Block statement and the
     * FunctionDeclaration end/body positions drift). Fixture
     * 107_generatorTypeCheck4.ts (`function* g1(): {} { }`) exercises this. */
    (void)parse_type_annotation(p);
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts
     * parseFunctionBlockOrSemicolon (~7567):
     *     if (token() !== OpenBraceToken) {
     *         if (canParseSemicolon()) { parseSemicolon(); return; }
     *     }
     *     return parseFunctionBlock(...);
     * canParseSemicolon (~2567) accepts SemicolonToken, CloseBraceToken,
     * EndOfFileToken, or any token preceded by a line break. When body is
     * left undefined (overload signature), transformers/ts.ts ~1555
     * visitFunctionDeclaration → shouldEmitFunctionLikeDeclaration ~1297
     * replaces the declaration with NotEmittedStatement, so it produces no
     * output in the printed JS.
     *
     * Otherwise parseFunctionBlock delegates to parseBlock (~6824). When
     * parseExpected(OpenBraceToken) fails (openBraceParsed=false) and
     * ignoreMissingOpenBrace=false, tsc still returns a Block — a
     * zero-width one built from createMissingList<Statement>(), with
     * pos=end=scanner.getTokenFullStart() (finishNode records the same
     * position because the scanner hasn't moved). The
     * 106_parserEqualsGreaterThanAfterFunction1.ts fixture hits this
     * branch: `function =>` leaves the scanner parked at `=>`, and the
     * Block is synthesised at fullStart(`=>`) = 27. */
    CtscNode* body = NULL;
    int body_end;
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        if (is_async_function) p->await_context_depth++;
        body = parse_block(p);
        if (is_async_function) p->await_context_depth--;
        body_end = body->end;
    } else if (can_parse_semicolon_here(p)) {
        accept(p, CTSC_SK_SemicolonToken);
        body_end = cur_full_start(p);
    } else {
        /* parseExpected(OpenBraceToken) fails here: emit the diagnostic,
         * do not advance, and synthesise a zero-width Block. Mirrors
         * parseBlock's `else { const statements = createMissingList<...>();
         * return withJSDoc(finishNode(factoryCreateBlock(statements, ...),
         * pos), hasJSDoc); }` path (~6841). */
        int bpos = cur_full_start(p);
        expect(p, CTSC_SK_OpenBraceToken);
        body = ctsc_node_new(p->arena, CTSC_SK_Block, bpos, bpos);
        ctsc_node_array_init(&body->data.block.statements);
        body_end = bpos;
    }
    *out_params   = params;
    *out_body     = body;
    *out_body_end = body_end;
}

static CtscNode* parse_function_declaration(Parser* p, bool is_async) {
    int fs = cur_full_start(p);
    advance(p); /* function */
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseFunctionDeclaration
     * (~7734):
     *   parseExpected(FunctionKeyword);
     *   const asteriskToken = parseOptionalToken(AsteriskToken);
     *   const name = ... parseBindingIdentifier();
     * The `*` marks a generator (`function * foo() {}`). The oracle's
     * FunctionDeclaration JSON shape (harness/src/oracle-ast.ts ~81) does
     * not serialise asteriskToken, so we consume it but do not store it.
     *
     * The name is always produced by parseBindingIdentifier
     * (~2684 -> createIdentifier ~2619), which materialises a zero-width
     * missing Identifier when the current token is not a BindingIdentifier
     * (see the 106_parserEqualsGreaterThanAfterFunction1.ts fixture:
     * `function =>` → name pos=end=fullStart(`=>`)). Anonymous `export default
     * function ()` (parseOptionalBindingIdentifier) is not modelled yet. */
    if (cur(p) == CTSC_SK_AsteriskToken) {
        advance(p);
    }
    /* Mirrors parser.ts parseBindingIdentifier (~2684) → createIdentifier
     * (~2648): when isBindingIdentifier() is true the keyword token is
     * consumed as an Identifier (originalKeywordKind is remembered but the
     * oracle only prints escapedText). isBindingIdentifier (~2308) returns
     * true for real Identifiers and any token > LastReservedWord — that
     * range spans the strict-mode reserved words (ImplementsKeyword,
     * LetKeyword, PackageKeyword, PrivateKeyword, ProtectedKeyword,
     * PublicKeyword, StaticKeyword, YieldKeyword) and every contextual /
     * TS keyword, so `function yield() {}` at script top-level (no yield
     * context tracked yet) is accepted with name="yield". When the token
     * is not a binding identifier (e.g. `function =>` from
     * 106_parserEqualsGreaterThanAfterFunction1.ts) createIdentifier
     * falls through createMissingNode(Identifier) producing a zero-width
     * Identifier at scanner.getTokenFullStart(). */
    CtscNode* name;
    if (is_binding_identifier_kind(cur(p))) {
        name = make_identifier_from_current(p);
    } else {
        name = make_missing_identifier(p);
    }
    /* parser.ts parseFunctionDeclaration (~7743): typeParameters before parameters. */
    CtscNodeArray type_parameters;
    ctsc_node_array_init(&type_parameters);
    (void)parse_type_parameters(p, &type_parameters);
    CtscNodeArray params;
    CtscNode* body;
    int body_end;
    parse_function_signature_and_body(p, &params, &body, &body_end, is_async);
    CtscNode* fn = ctsc_node_new(p->arena, CTSC_SK_FunctionDeclaration, fs, body_end);
    fn->data.functionDeclaration.has_async = is_async;
    fn->data.functionDeclaration.name = name;
    fn->data.functionDeclaration.type_parameters = type_parameters;
    fn->data.functionDeclaration.parameters = params;
    fn->data.functionDeclaration.body = body;
    return fn;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseFunctionExpression
 * (~6765):
 *     parseExpected(FunctionKeyword);
 *     const asteriskToken = parseOptionalToken(AsteriskToken);
 *     const name = parseOptionalBindingIdentifier();
 *     ... parseParameters ... parseReturnType ... parseFunctionBlock ...
 *     factory.createFunctionExpression(...)
 * The only shape difference from parse_function_declaration is that the
 * name is optional: parseOptionalBindingIdentifier (~6797) returns
 * undefined when the current token is not a BindingIdentifier, so for
 * `++function(e) { }` (fixture 107_parserUnaryExpression2.ts) the name
 * slot is NULL and the parameter list starts immediately after `function`.
 *
 * We reuse CtscFunctionDeclarationData since the FunctionExpression shape
 * is identical (name / parameters / body); only CtscNode.kind distinguishes
 * the two, which matches tsc's forEachChildInFunctionLikeDeclaration
 * visitor (parser.ts ~548) — a single visitor for both kinds.
 */
static CtscNode* parse_function_expression(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* function */
    /* parseOptionalToken(AsteriskToken) (parser.ts ~6765). When the `*` is
     * present we record its pos/end so ast_json.c can emit the AsteriskToken
     * child leaf. Mirrors parseTokenNode (~2553): pos = getNodePos() BEFORE
     * consuming (== full_start of `*`), end = scanner.getTokenFullStart()
     * AFTER nextToken() (== full_start of the token that follows `*`, which
     * includes any trailing trivia between `*` and the next token). For
     * `function * () { }` (fixture 108_FunctionExpression1_es6.ts) that is
     * pos=33, end=35 (space at 34 is part of the NEXT token's leading
     * trivia, so full_start of `(` is 35). */
    bool has_asterisk = false;
    int asterisk_pos = 0, asterisk_end = 0;
    if (cur(p) == CTSC_SK_AsteriskToken) {
        has_asterisk = true;
        asterisk_pos = cur_full_start(p);
        advance(p);
        asterisk_end = cur_full_start(p);
    }
    /* parseOptionalBindingIdentifier (parser.ts ~6797): Identifier only when
     * isBindingIdentifier() is true; otherwise NULL (undefined). */
    CtscNode* name = NULL;
    if (is_binding_identifier_kind(cur(p))) {
        name = make_identifier_from_current(p);
    }
    CtscNodeArray type_parameters;
    ctsc_node_array_init(&type_parameters);
    (void)parse_type_parameters(p, &type_parameters);
    CtscNodeArray params;
    CtscNode* body;
    int body_end;
    parse_function_signature_and_body(p, &params, &body, &body_end, false);
    CtscNode* fn = ctsc_node_new(p->arena, CTSC_SK_FunctionExpression, fs, body_end);
    fn->data.functionDeclaration.has_asterisk = has_asterisk;
    fn->data.functionDeclaration.asterisk_pos = asterisk_pos;
    fn->data.functionDeclaration.asterisk_end = asterisk_end;
    fn->data.functionDeclaration.name = name;
    fn->data.functionDeclaration.type_parameters = type_parameters;
    fn->data.functionDeclaration.parameters = params;
    fn->data.functionDeclaration.body = body;
    return fn;
}

static CtscNode* parse_if_statement(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* if */
    expect(p, CTSC_SK_OpenParenToken);
    CtscNode* expr = parse_expression(p);
    expect(p, CTSC_SK_CloseParenToken);
    CtscNode* thenS = parse_statement(p);
    CtscNode* elseS = NULL;
    if (cur(p) == CTSC_SK_ElseKeyword) {
        advance(p);
        elseS = parse_statement(p);
    }
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_IfStatement, fs, (elseS ? elseS->end : (thenS ? thenS->end : cur_end(p))));
    n->data.ifStatement.expression = expr;
    n->data.ifStatement.thenStatement = thenS;
    n->data.ifStatement.elseStatement = elseS;
    return n;
}

static CtscNode* parse_while_statement(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* while */
    expect(p, CTSC_SK_OpenParenToken);
    CtscNode* expr = parse_expression(p);
    expect(p, CTSC_SK_CloseParenToken);
    CtscNode* body = parse_statement(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_WhileStatement, fs, body ? body->end : cur_end(p));
    n->data.whileStatement.expression = expr;
    n->data.whileStatement.statement = body;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseDoStatement (~6897):
 *     parseExpected(DoKeyword);
 *     const statement = parseStatement();
 *     parseExpected(WhileKeyword);
 *     parseExpected(OpenParenToken);
 *     const expression = allowInAnd(parseExpression);
 *     parseExpected(CloseParenToken);
 *     parseOptional(SemicolonToken);  // de-facto ASI carve-out
 * finishNode sets end = scanner.getTokenFullStart() of the NEXT token after
 * the optionally-consumed `;`. For `do{;}while(false)false` the `;` is
 * absent, so end = full_start of the following `false` ExpressionStatement
 * (== 36), matching the oracle.
 */
/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseWithStatement (~7000):
 *     parseExpected(WithKeyword);
 *     parseExpected(OpenParenToken);
 *     const expression = allowInAnd(parseExpression);
 *     parseExpectedMatchingBrackets(OpenParen, CloseParen, ...);
 *     const statement = doInsideOfContext(NodeFlags.InWithStatement, parseStatement);
 * ctsc does not yet model NodeFlags, but parse_statement is called directly
 * which suffices for byte-for-byte AST parity (InWithStatement is a flag
 * consumed by the binder/checker, not the serialiser).
 */
static CtscNode* parse_with_statement(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* with */
    expect(p, CTSC_SK_OpenParenToken);
    CtscNode* expr = parse_expression(p);
    expect(p, CTSC_SK_CloseParenToken);
    CtscNode* body = parse_statement(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_WithStatement, fs, body ? body->end : cur_end(p));
    n->data.withStatement.expression = expr;
    n->data.withStatement.statement = body;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isListTerminator for
 * ParsingContext.SwitchClauseStatements (~3017): `}` / `case` / `default`
 * end the statement list inside a clause.
 */
static bool is_switch_clause_statement_list_terminator(const Parser* p) {
    CtscSyntaxKind k = cur(p);
    if (k == CTSC_SK_EndOfFileToken) return true;
    if (k == CTSC_SK_CloseBraceToken) return true;
    if (k == CTSC_SK_CaseKeyword) return true;
    if (k == CTSC_SK_DefaultKeyword) return true;
    return false;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseCaseClause (~7012).
 */
static CtscNode* parse_case_clause(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_CaseKeyword);
    CtscNode* expr = parse_expression(p);
    expect(p, CTSC_SK_ColonToken);
    CtscNodeArray stmts;
    ctsc_node_array_init(&stmts);
    while (!is_switch_clause_statement_list_terminator(p)) {
        int before = (int)p->scanner.pos;
        CtscNode* s = parse_statement(p);
        if (s) ctsc_node_array_push(&stmts, p->arena, s);
        if ((int)p->scanner.pos == before) advance(p);
    }
    int end = stmts.len > 0 ? stmts.items[stmts.len - 1]->end : cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_CaseClause, fs, end);
    n->data.caseClause.expression = expr;
    n->data.caseClause.statements = stmts;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseDefaultClause (~7022).
 */
static CtscNode* parse_default_clause(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_DefaultKeyword);
    expect(p, CTSC_SK_ColonToken);
    CtscNodeArray stmts;
    ctsc_node_array_init(&stmts);
    while (!is_switch_clause_statement_list_terminator(p)) {
        int before = (int)p->scanner.pos;
        CtscNode* s = parse_statement(p);
        if (s) ctsc_node_array_push(&stmts, p->arena, s);
        if ((int)p->scanner.pos == before) advance(p);
    }
    int end = stmts.len > 0 ? stmts.items[stmts.len - 1]->end : cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_DefaultClause, fs, end);
    n->data.defaultClause.statements = stmts;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseCaseBlock (~7034).
 */
static CtscNode* parse_case_block(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_OpenBraceToken);
    bool multi_line = p->scanner.current.has_preceding_line_break;
    CtscNodeArray clauses;
    ctsc_node_array_init(&clauses);
    while (cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken) {
        if (cur(p) == CTSC_SK_CaseKeyword) {
            CtscNode* c = parse_case_clause(p);
            if (c) ctsc_node_array_push(&clauses, p->arena, c);
        } else if (cur(p) == CTSC_SK_DefaultKeyword) {
            CtscNode* d = parse_default_clause(p);
            if (d) ctsc_node_array_push(&clauses, p->arena, d);
        } else {
            advance(p);
        }
    }
    expect(p, CTSC_SK_CloseBraceToken);
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_CaseBlock, fs, end);
    n->data.caseBlock.clauses = clauses;
    n->data.caseBlock.multi_line = multi_line;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseSwitchStatement (~7042).
 */
static CtscNode* parse_switch_statement(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* switch */
    expect(p, CTSC_SK_OpenParenToken);
    CtscNode* expr = parse_expression(p);
    expect(p, CTSC_SK_CloseParenToken);
    CtscNode* cb = parse_case_block(p);
    int end = cb ? cb->end : cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_SwitchStatement, fs, end);
    n->data.switchStatement.expression = expr;
    n->data.switchStatement.caseBlock = cb;
    return n;
}

static CtscNode* parse_do_statement(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* do */
    CtscNode* body = parse_statement(p);
    expect(p, CTSC_SK_WhileKeyword);
    expect(p, CTSC_SK_OpenParenToken);
    CtscNode* expr = parse_expression(p);
    expect(p, CTSC_SK_CloseParenToken);
    accept(p, CTSC_SK_SemicolonToken); /* parseOptional */
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_DoStatement, fs, end);
    n->data.doStatement.statement = body;
    n->data.doStatement.expression = expr;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts canFollowContextualOfKeyword
 * (~7723):
 *     function canFollowContextualOfKeyword(): boolean {
 *         return nextTokenIsIdentifier() && nextToken() === SyntaxKind.CloseParenToken;
 *     }
 * Used inside parseVariableDeclarationList (~7705) to detect the
 * `for (var of X)` shape: the user typed `of` where a binding name was
 * expected, and the token after that is an Identifier followed by `)`. In
 * that case tsc abandons the VariableDeclarations list (declarations ends
 * empty via createMissingList) so that `of` can be re-consumed as the
 * contextual for-of keyword by parseForOrForInOrForOfStatement.
 *
 * isIdentifier (~2318) also accepts any contextual/TS keyword (anything
 * past LastReservedWord); we mirror that via is_binding_identifier_start
 * minus the binding-pattern starters ({ / [).
 */
static bool look_ahead_can_follow_contextual_of(Parser* p) {
    CtscScanner saved = p->scanner;
    advance(p); /* past `of` */
    CtscSyntaxKind after_of = cur(p);
    /* nextTokenIsIdentifier: token() === Identifier or token() > LastReservedWord. */
    bool after_is_id = (after_of == CTSC_SK_Identifier)
        || (after_of >= CTSC_SK_AsKeyword && after_of <= CTSC_SK_UnknownKeyword);
    bool ok = false;
    if (after_is_id) {
        advance(p); /* past the identifier */
        ok = cur(p) == CTSC_SK_CloseParenToken;
    }
    p->scanner = saved;
    return ok;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseForOrForInOrForOfStatement
 * (~6928). After parsing the initializer (either a VariableDeclarationList or
 * an expression, or nothing when the first token is already `;`), inspect the
 * next token:
 *   - `of`  -> ForOfStatement(initializer, expression, statement)
 *   - `in`  -> ForInStatement(initializer, expression, statement)
 *   - else  -> classic ForStatement with `; cond ; inc ;` tail
 *
 * For the ForIn / contextual-of path, parseVariableDeclarationList (~7666)
 * tolerates an empty declarations list: when the token after
 * `var`/`let`/`const` is not a valid binding identifier start (or is the
 * contextual `of` keyword per canFollowContextualOfKeyword ~7723), the list
 * ends at the full-start of the offending token. Fixture
 * 107_parserForInStatement2.ts (`for (var in X) {}`) and
 * 107_parserForOfStatement2.ts (`for (var of X) {}`) exercise this — the
 * VariableDeclarationList spans only the `var` keyword + trailing trivia.
 */
static CtscNode* parse_for_statement(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* for */
    expect(p, CTSC_SK_OpenParenToken);
    CtscNode* init = NULL;
    if (cur(p) == CTSC_SK_VarKeyword || cur(p) == CTSC_SK_LetKeyword || cur(p) == CTSC_SK_ConstKeyword) {
        /* variable declaration list without trailing ';' */
        int flags = 0;
        if      (cur(p) == CTSC_SK_LetKeyword)   flags |= 1;
        else if (cur(p) == CTSC_SK_ConstKeyword) flags |= 2;
        int list_fs = cur_full_start(p);
        advance(p);
        CtscNodeArray decls; ctsc_node_array_init(&decls);
        /* Mirrors parseVariableDeclarationList (~7705):
         *     if (token() === OfKeyword && lookAhead(canFollowContextualOfKeyword)) {
         *         declarations = createMissingList<VariableDeclaration>();
         *     }
         * That branch leaves `of` unconsumed so the enclosing for-head can
         * pick it up as the for-of keyword. */
        bool contextual_of = cur(p) == CTSC_SK_OfKeyword
                          && look_ahead_can_follow_contextual_of(p);
        if (!contextual_of) {
            /* Mirrors parseDelimitedList(VariableDeclarations) (~3489): only
             * enter the parse-element branch when the current token can start
             * a binding identifier. Otherwise leave decls empty — this is the
             * tsc recovery path for malformed `for (var in X)`. */
            for (;;) {
                if (!is_binding_identifier_start(cur(p))) break;
                CtscNode* d = parse_variable_declaration(p);
                if (!d) break;
                ctsc_node_array_push(&decls, p->arena, d);
                if (!accept(p, CTSC_SK_CommaToken)) break;
            }
        }
        /* Mirrors parser.ts finishNode (~2600): end = scanner.getTokenFullStart()
         * of the next token, matching tsc's VariableDeclarationList layout. */
        int list_end = cur_full_start(p);
        CtscNode* list = ctsc_node_new(p->arena, CTSC_SK_VariableDeclarationList, list_fs, list_end);
        list->data.variableDeclarationList.flags = flags;
        list->data.variableDeclarationList.declarations = decls;
        init = list;
    } else if (cur(p) != CTSC_SK_SemicolonToken) {
        init = parse_expression(p);
    }
    /* Mirrors parser.ts (~6956): if the next token is `in`, build ForInStatement.
     * ForOfStatement handled symmetrically for parity, though no fixture has
     * unlocked it yet. */
    if (cur(p) == CTSC_SK_InKeyword || cur(p) == CTSC_SK_OfKeyword) {
        CtscSyntaxKind kw = cur(p);
        advance(p); /* consume `in` / `of` */
        CtscNode* expr = parse_expression(p);
        expect(p, CTSC_SK_CloseParenToken);
        CtscNode* body = parse_statement(p);
        CtscSyntaxKind kind = (kw == CTSC_SK_InKeyword) ? CTSC_SK_ForInStatement : CTSC_SK_ForOfStatement;
        CtscNode* n = ctsc_node_new(p->arena, kind, fs, body ? body->end : cur_end(p));
        n->data.forInOrOfStatement.initializer = init;
        n->data.forInOrOfStatement.expression  = expr;
        n->data.forInOrOfStatement.statement   = body;
        return n;
    }
    expect(p, CTSC_SK_SemicolonToken);
    CtscNode* cond = NULL;
    if (cur(p) != CTSC_SK_SemicolonToken) cond = parse_expression(p);
    expect(p, CTSC_SK_SemicolonToken);
    CtscNode* inc = NULL;
    if (cur(p) != CTSC_SK_CloseParenToken) inc = parse_expression(p);
    expect(p, CTSC_SK_CloseParenToken);
    CtscNode* body = parse_statement(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_ForStatement, fs, body ? body->end : cur_end(p));
    n->data.forStatement.initializer = init;
    n->data.forStatement.condition   = cond;
    n->data.forStatement.incrementor = inc;
    n->data.forStatement.statement   = body;
    return n;
}

static CtscNode* parse_return_statement(Parser* p) {
    int fs = cur_full_start(p);
    int end = cur_end(p);
    advance(p); /* return */
    CtscNode* expr = NULL;
    if (cur(p) != CTSC_SK_SemicolonToken && cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken
        && !p->scanner.current.has_preceding_line_break) {
        expr = parse_expression(p);
        if (expr) end = expr->end;
    }
    if (cur(p) == CTSC_SK_SemicolonToken) { end = cur_end(p); advance(p); }
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_ReturnStatement, fs, end);
    n->data.returnStatement.expression = expr;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseThrowStatement (~7053):
 *     parseExpected(ThrowKeyword);
 *     let expression = scanner.hasPrecedingLineBreak() ? undefined : allowInAnd(parseExpression);
 *     if (expression === undefined) {
 *         expression = finishNode(factoryCreateIdentifier(""), getNodePos());
 *     }
 *     tryParseSemicolon();
 */
static CtscNode* parse_throw_statement(Parser* p) {
    int fs = cur_full_start(p);
    int end = cur_end(p);
    advance(p); /* throw */
    CtscNode* expr = NULL;
    if (!p->scanner.current.has_preceding_line_break) {
        expr = parse_expression(p);
        if (expr) end = expr->end;
    }
    if (!expr) {
        expr = make_missing_identifier(p);
        end = expr->end;
    }
    if (cur(p) == CTSC_SK_SemicolonToken) { end = cur_end(p); advance(p); }
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_ThrowStatement, fs, end);
    n->data.throwStatement.expression = expr;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseDebuggerStatement (~7115):
 *   const pos = getNodePos();
 *   parseExpected(SyntaxKind.DebuggerKeyword);
 *   parseSemicolon();
 *   return finishNode(factory.createDebuggerStatement(), pos);
 * finishNode (~2600) sets `end = scanner.getTokenFullStart()` of the next token,
 * which is `cur_full_start(p)` after the keyword and optional `;` are consumed
 * (parseSemicolon / canParseSemicolon ~2567 allows ASI at `}`, EOF, or after a
 * preceding line break — ctsc does not need an explicit check because
 * cur_full_start captures the same value in all three ASI branches).
 *
 * DebuggerStatement has no payload children (forEachChildInDebuggerStatement
 * visits nothing), and the oracle (harness/src/oracle-ast.ts) falls through to
 * its default branch which emits only {kind,pos,end} when `children` is empty.
 */
static CtscNode* parse_debugger_statement(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* debugger */
    if (cur(p) == CTSC_SK_SemicolonToken) { advance(p); }
    int end = cur_full_start(p);
    return ctsc_node_new(p->arena, CTSC_SK_DebuggerStatement, fs, end);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseBreakOrContinueStatement (~6977)
 * and canParseSemicolon (~2567): parse the keyword, an optional label identifier (only
 * when a semicolon is NOT insertable via ASI or explicit ';'), then the terminator. The
 * finishNode end (== scanner.getTokenFullStart() at return time) includes the consumed
 * semicolon.
 */
static CtscNode* parse_break_or_continue_statement(Parser* p, CtscSyntaxKind kind) {
    int fs = cur_full_start(p);
    int end = cur_end(p);
    advance(p); /* break / continue */
    CtscNode* label = NULL;
    bool can_parse_semicolon = cur(p) == CTSC_SK_SemicolonToken
        || cur(p) == CTSC_SK_CloseBraceToken
        || cur(p) == CTSC_SK_EndOfFileToken
        || p->scanner.current.has_preceding_line_break;
    if (!can_parse_semicolon) {
        label = parse_identifier(p);
        if (label) end = label->end;
    }
    if (cur(p) == CTSC_SK_SemicolonToken) { end = cur_end(p); advance(p); }
    CtscNode* n = ctsc_node_new(p->arena, kind, fs, end);
    n->data.breakOrContinueStatement.label = label;
    return n;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts isStartOfStatement
 * (~7316-7324) for the TS-only modifier keywords
 * (Public/Private/Protected/Static/Readonly/Accessor):
 *
 *     case SyntaxKind.AccessorKeyword:
 *     case SyntaxKind.PublicKeyword:
 *     case SyntaxKind.PrivateKeyword:
 *     case SyntaxKind.ProtectedKeyword:
 *     case SyntaxKind.StaticKeyword:
 *     case SyntaxKind.ReadonlyKeyword:
 *         // When these don't start a declaration, they may be the start
 *         // of a class member if an identifier immediately follows.
 *         // Otherwise they're an identifier in an expression statement.
 *         return isStartOfDeclaration() || !lookAhead(nextTokenIsIdentifierOrKeywordOnSameLine);
 *
 * The condition under which `isStartOfStatement` returns FALSE (i.e. the
 * SourceElements / BlockStatements list's `isListElement` check rejects the
 * current token and `parseList` dispatches to
 * `abortParsingListOrMoveToNextToken` (~3410): report
 * "Declaration or statement expected." (Diagnostics 1128) and `nextToken()`)
 * is: `!isStartOfDeclaration() && nextTokenIsIdentifierOrKeywordOnSameLine`.
 *
 * ctsc does not yet model `parseDeclaration` for TS declarations with
 * modifiers (no fixture requires it); in every fixture we currently see a
 * modifier in statement position it is NOT starting a declaration. So this
 * helper only needs to compute the lookahead arm: if the next token is an
 * identifier-or-keyword (tsc scanner.ts tokenIsIdentifierOrKeyword ~42:
 * `token >= Identifier`) on the same line, the modifier is a stray
 * statement-start that the parseList loop must skip. Fixture
 * 107_parserPublicBreak1.ts (`public break;`) exercises this path.
 */
static bool next_token_is_identifier_or_keyword_on_same_line(Parser* p) {
    CtscScanner saved = p->scanner;
    advance(p);
    bool ok = (cur(p) >= CTSC_SK_Identifier)
              && !p->scanner.current.has_preceding_line_break;
    p->scanner = saved;
    return ok;
}

static bool is_ts_modifier_keyword(CtscSyntaxKind k) {
    switch (k) {
        case CTSC_SK_PublicKeyword:
        case CTSC_SK_PrivateKeyword:
        case CTSC_SK_ProtectedKeyword:
        case CTSC_SK_StaticKeyword:
        case CTSC_SK_ReadonlyKeyword:
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseModifiers
         * (~8015): AbstractKeyword is one of the TS modifier keywords
         * (canFollowModifier ~7222 returns true for it unless separated by
         * a line break). Needed so parse_modifiers consumes `abstract` as
         * a leaf modifier node preceding a class / interface declaration
         * (108_classAbstractWithInterface.ts: `abstract interface I {}`). */
        case CTSC_SK_AbstractKeyword:
            return true;
        default:
            return false;
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseModifiers (~8015)
 * at the leading-modifier phase only (decorators and trailing-decorator
 * round-trips are not yet modelled; no active fixture exercises them).
 * Each modifier is a bare token leaf built by factoryCreateToken(kind) +
 * finishNode (~7980 tryParseModifier → ~8003), so pos = full_start before
 * consuming the keyword and end = scanner.getTokenFullStart() after
 * consuming it (== ctsc's cur_full_start(p) post-advance).
 *
 * Only the subset of modifiers that the declaration arms currently dispatch
 * on is accepted (the TS accessibility keywords). When a fixture needs
 * AbstractKeyword / DeclareKeyword / AsyncKeyword / ExportKeyword / ...,
 * extend the is_ts_modifier_keyword predicate alongside the dispatcher.
 */
static void parse_modifiers(Parser* p, CtscNodeArray* out) {
    while (is_ts_modifier_keyword(cur(p))) {
        CtscSyntaxKind k = cur(p);
        int mpos = cur_full_start(p);
        advance(p);
        int mend = cur_full_start(p);
        CtscNode* m = ctsc_node_new(p->arena, k, mpos, mend);
        ctsc_node_array_push(out, p->arena, m);
    }
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * nextTokenIsIdentifierOrStringLiteralOnSameLine (~7562), used by
 * isStartOfDeclaration (~7208) to decide whether a `module`/`namespace`
 * contextual keyword opens a ModuleDeclaration vs. an expression-statement
 * identifier reference. `isIdentifier()` in tsc returns true for real
 * identifiers and for any token past LastReservedWord (contextual / TS
 * keywords), so ctsc mirrors that with the is_binding_identifier_kind
 * classifier.
 */
static bool next_token_is_identifier_or_string_literal_on_same_line(Parser* p) {
    CtscScanner saved = p->scanner;
    advance(p);
    bool ok = !p->scanner.current.has_preceding_line_break
              && (is_binding_identifier_kind(cur(p))
                  || cur(p) == CTSC_SK_StringLiteral);
    p->scanner = saved;
    return ok;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseImportSpecifier (~8600)
 * / parseImportOrExportSpecifier (~8604).
 */
static CtscNode* parse_import_specifier_name(Parser* p) {
    if (!token_is_identifier_or_keyword(cur(p))) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1003, cur_start(p), cur_end(p) - cur_start(p),
            "Identifier expected.");
        return NULL;
    }
    return make_identifier_from_current(p);
}

static bool import_specifier_identifier_is_ascii_exact(const CtscNode* id, const char* ascii) {
    if (!id || id->kind != CTSC_SK_Identifier) return false;
    const uint16_t* t = id->data.identifier.text;
    size_t len = id->data.identifier.text_len;
    for (size_t i = 0;; ++i) {
        unsigned char c = (unsigned char)ascii[i];
        if (c == 0) return i == len;
        if (i >= len || t[i] != (uint16_t)c) return false;
    }
}

static bool can_parse_module_export_name_for_import(Parser* p) {
    return token_is_identifier_or_keyword(cur(p)) || cur(p) == CTSC_SK_StringLiteral;
}

/*
 * Mirrors upstream parser.ts parseImportOrExportSpecifier (~8604) for
 * ImportSpecifier (including `type` / `type as` disambiguation).
 */
static CtscNode* parse_import_specifier(Parser* p) {
    int fs = cur_full_start(p);
    bool is_type_only = false;
    CtscNode* property_name = NULL;
    CtscNode* name = parse_import_specifier_name(p);
    if (!name) return NULL;
    bool can_parse_as_keyword = true;

    if (import_specifier_identifier_is_ascii_exact(name, "type")) {
        if (cur(p) == CTSC_SK_AsKeyword) {
            CtscNode* first_as = parse_import_specifier_name(p);
            if (!first_as) return NULL;
            if (cur(p) == CTSC_SK_AsKeyword) {
                CtscNode* second_as = parse_import_specifier_name(p);
                if (!second_as) return NULL;
                if (can_parse_module_export_name_for_import(p)) {
                    is_type_only = true;
                    property_name = first_as;
                    name = parse_import_specifier_name(p);
                    if (!name) return NULL;
                    can_parse_as_keyword = false;
                } else {
                    property_name = name;
                    name = second_as;
                    can_parse_as_keyword = false;
                }
            } else if (can_parse_module_export_name_for_import(p)) {
                property_name = name;
                can_parse_as_keyword = false;
                name = parse_import_specifier_name(p);
                if (!name) return NULL;
            } else {
                is_type_only = true;
                name = first_as;
            }
        } else if (can_parse_module_export_name_for_import(p)) {
            is_type_only = true;
            name = parse_import_specifier_name(p);
            if (!name) return NULL;
            can_parse_as_keyword = false;
        }
    }

    if (can_parse_as_keyword && cur(p) == CTSC_SK_AsKeyword) {
        property_name = name;
        if (!expect(p, CTSC_SK_AsKeyword)) return NULL;
        name = parse_import_specifier_name(p);
        if (!name) return NULL;
    }

    int end = name->end;
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_ImportSpecifier, fs, end);
    n->data.importSpecifier.is_type_only = is_type_only;
    n->data.importSpecifier.propertyName = property_name;
    n->data.importSpecifier.name = name;
    return n;
}

/*
 * Mirrors upstream parser.ts parseNamedImportsOrExports (~8578) for NamedImports.
 */
static CtscNode* parse_named_imports(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_OpenBraceToken);
    CtscNodeArray elements;
    ctsc_node_array_init(&elements);
    while (cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken) {
        CtscNode* el = parse_import_specifier(p);
        if (!el) break;
        ctsc_node_array_push(&elements, p->arena, el);
        if (accept(p, CTSC_SK_CommaToken)) continue;
        break;
    }
    expect(p, CTSC_SK_CloseBraceToken);
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_NamedImports, fs, end);
    n->data.namedImports.elements = elements;
    return n;
}

static CtscNode* parse_module_specifier_string(Parser* p) {
    if (cur(p) != CTSC_SK_StringLiteral) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005, cur_start(p), cur_end(p) - cur_start(p),
            "String literal expected.");
        return NULL;
    }
    int fs = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_StringLiteral, fs, cur_end(p));
    n->data.stringLiteral.text = p->scanner.current.text;
    n->data.stringLiteral.text_len = p->scanner.current.text_len;
    n->data.stringLiteral.value = p->scanner.current.value;
    n->data.stringLiteral.value_len = p->scanner.current.value_len;
    n->data.stringLiteral.single_quote = (p->scanner.current.text_len > 0 && p->scanner.current.text[0] == '\'');
    advance(p);
    return n;
}

/*
 * Pre: scanner positioned at `{` opening named imports; parses through module specifier
 * and optional semicolon. Mirrors parser.ts parseImportDeclaration (~8384) for
 * `import { ... } from "..."`.
 */
static CtscNode* parse_import_declaration_named_from_brace(Parser* p, int decl_fs, bool clause_is_type_only) {
    CtscNode* named = parse_named_imports(p);
    if (!named) return NULL;
    CtscNode* clause = ctsc_node_new(p->arena, CTSC_SK_ImportClause, named->pos, named->end);
    clause->data.importClause.is_type_only = clause_is_type_only;
    clause->data.importClause.name = NULL;
    clause->data.importClause.namedBindings = named;
    if (!expect(p, CTSC_SK_FromKeyword)) return NULL;
    CtscNode* mod = parse_module_specifier_string(p);
    if (!mod) return NULL;
    int decl_end = mod->end;
    if (cur(p) == CTSC_SK_SemicolonToken) {
        decl_end = cur_end(p);
        advance(p);
    }
    CtscNode* decl = ctsc_node_new(p->arena, CTSC_SK_ImportDeclaration, decl_fs, decl_end);
    decl->data.importDeclaration.importClause = clause;
    decl->data.importDeclaration.moduleSpecifier = mod;
    return decl;
}

/*
 * Pre: `import` keyword consumed. Mirrors parser.ts
 * parseImportDeclarationOrImportEqualsDeclaration (~8384),
 * tryParseImportClause (~8425), parseImportClause (~8501),
 * parseNamespaceImport (~8558). Does not handle import equals or import attributes.
 */
static CtscNode* parse_import_declaration_after_import_keyword(Parser* p, int decl_fs) {
    bool clause_is_type_only = false;
    /* Mirrors upstream parser.ts parseImportDeclarationOrImportEqualsDeclaration
     * (~8395-8402): `import type` sets ImportClause phase modifier / isTypeOnly. */
    if (cur(p) == CTSC_SK_TypeKeyword && !p->scanner.current.has_preceding_line_break) {
        advance(p);
        clause_is_type_only = true;
    }
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        return parse_import_declaration_named_from_brace(p, decl_fs, clause_is_type_only);
    }
    if (cur(p) == CTSC_SK_StringLiteral) {
        /* import ModuleSpecifier ; */
        CtscNode* mod = parse_module_specifier_string(p);
        if (!mod) return NULL;
        int decl_end = mod->end;
        if (cur(p) == CTSC_SK_SemicolonToken) {
            decl_end = cur_end(p);
            advance(p);
        }
        CtscNode* decl = ctsc_node_new(p->arena, CTSC_SK_ImportDeclaration, decl_fs, decl_end);
        decl->data.importDeclaration.importClause = NULL;
        decl->data.importDeclaration.moduleSpecifier = mod;
        return decl;
    }
    if (cur(p) == CTSC_SK_AsteriskToken) {
        int ns_fs = cur_full_start(p);
        advance(p);
        if (!expect(p, CTSC_SK_AsKeyword)) return NULL;
        CtscNode* bind_name = parse_import_specifier_name(p);
        if (!bind_name) return NULL;
        CtscNode* ns = ctsc_node_new(p->arena, CTSC_SK_NamespaceImport, ns_fs, bind_name->end);
        ns->data.namespaceImport.name = bind_name;
        if (!expect(p, CTSC_SK_FromKeyword)) return NULL;
        CtscNode* mod = parse_module_specifier_string(p);
        if (!mod) return NULL;
        int decl_end = mod->end;
        if (cur(p) == CTSC_SK_SemicolonToken) {
            decl_end = cur_end(p);
            advance(p);
        }
        CtscNode* clause = ctsc_node_new(p->arena, CTSC_SK_ImportClause, ns->pos, ns->end);
        clause->data.importClause.is_type_only = clause_is_type_only;
        clause->data.importClause.name = NULL;
        clause->data.importClause.namedBindings = ns;
        CtscNode* decl = ctsc_node_new(p->arena, CTSC_SK_ImportDeclaration, decl_fs, decl_end);
        decl->data.importDeclaration.importClause = clause;
        decl->data.importDeclaration.moduleSpecifier = mod;
        return decl;
    }
    if (token_is_identifier_or_keyword(cur(p))) {
        /*
         * ImportedDefaultBinding [`, ` (NameSpaceImport | NamedImports)] `from`
         * ModuleSpecifier `;`
         * Mirrors upstream parser.ts parseImportClause (~8501-8526): after an
         * optional default binding, a comma introduces namespace or named imports.
         */
        CtscNode* def = parse_import_specifier_name(p);
        if (!def) return NULL;
        CtscNode* named_bindings = NULL;
        if (accept(p, CTSC_SK_CommaToken)) {
            if (cur(p) == CTSC_SK_AsteriskToken) {
                int ns_fs = cur_full_start(p);
                advance(p);
                if (!expect(p, CTSC_SK_AsKeyword)) return NULL;
                CtscNode* bind_name = parse_import_specifier_name(p);
                if (!bind_name) return NULL;
                CtscNode* ns = ctsc_node_new(p->arena, CTSC_SK_NamespaceImport, ns_fs, bind_name->end);
                ns->data.namespaceImport.name = bind_name;
                named_bindings = ns;
            } else if (cur(p) == CTSC_SK_OpenBraceToken) {
                named_bindings = parse_named_imports(p);
                if (!named_bindings) return NULL;
            } else {
                ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1005, cur_start(p), cur_end(p) - cur_start(p),
                    "'{' or '*' expected.");
                return NULL;
            }
        }
        if (!expect(p, CTSC_SK_FromKeyword)) return NULL;
        CtscNode* mod = parse_module_specifier_string(p);
        if (!mod) return NULL;
        int decl_end = mod->end;
        if (cur(p) == CTSC_SK_SemicolonToken) {
            decl_end = cur_end(p);
            advance(p);
        }
        int clause_end = named_bindings ? named_bindings->end : def->end;
        CtscNode* clause = ctsc_node_new(p->arena, CTSC_SK_ImportClause, def->pos, clause_end);
        clause->data.importClause.is_type_only = clause_is_type_only;
        clause->data.importClause.name = def;
        clause->data.importClause.namedBindings = named_bindings;
        CtscNode* decl = ctsc_node_new(p->arena, CTSC_SK_ImportDeclaration, decl_fs, decl_end);
        decl->data.importDeclaration.importClause = clause;
        decl->data.importDeclaration.moduleSpecifier = mod;
        return decl;
    }
    return NULL;
}

/*
 * Same shape as parse_import_specifier (~5328) but ExportSpecifier
 * (parser.ts parseImportOrExportSpecifier ~8604).
 */
static CtscNode* parse_export_specifier(Parser* p) {
    int fs = cur_full_start(p);
    bool is_type_only = false;
    CtscNode* property_name = NULL;
    CtscNode* name = parse_import_specifier_name(p);
    if (!name) return NULL;
    bool can_parse_as_keyword = true;

    if (import_specifier_identifier_is_ascii_exact(name, "type")) {
        if (cur(p) == CTSC_SK_AsKeyword) {
            CtscNode* first_as = parse_import_specifier_name(p);
            if (!first_as) return NULL;
            if (cur(p) == CTSC_SK_AsKeyword) {
                CtscNode* second_as = parse_import_specifier_name(p);
                if (!second_as) return NULL;
                if (can_parse_module_export_name_for_import(p)) {
                    is_type_only = true;
                    property_name = first_as;
                    name = parse_import_specifier_name(p);
                    if (!name) return NULL;
                    can_parse_as_keyword = false;
                } else {
                    property_name = name;
                    name = second_as;
                    can_parse_as_keyword = false;
                }
            } else if (can_parse_module_export_name_for_import(p)) {
                property_name = name;
                can_parse_as_keyword = false;
                name = parse_import_specifier_name(p);
                if (!name) return NULL;
            } else {
                is_type_only = true;
                name = first_as;
            }
        } else if (can_parse_module_export_name_for_import(p)) {
            is_type_only = true;
            name = parse_import_specifier_name(p);
            if (!name) return NULL;
            can_parse_as_keyword = false;
        }
    }

    if (can_parse_as_keyword && cur(p) == CTSC_SK_AsKeyword) {
        property_name = name;
        if (!expect(p, CTSC_SK_AsKeyword)) return NULL;
        name = parse_import_specifier_name(p);
        if (!name) return NULL;
    }

    int end = name->end;
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_ExportSpecifier, fs, end);
    n->data.exportSpecifier.is_type_only = is_type_only;
    n->data.exportSpecifier.propertyName = property_name;
    n->data.exportSpecifier.name = name;
    return n;
}

/*
 * Mirrors parser.ts parseNamedImportsOrExports (~8578) for NamedExports.
 */
static CtscNode* parse_named_exports(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_OpenBraceToken);
    CtscNodeArray elements;
    ctsc_node_array_init(&elements);
    while (cur(p) != CTSC_SK_CloseBraceToken && cur(p) != CTSC_SK_EndOfFileToken) {
        CtscNode* el = parse_export_specifier(p);
        if (!el) break;
        ctsc_node_array_push(&elements, p->arena, el);
        if (accept(p, CTSC_SK_CommaToken)) continue;
        break;
    }
    expect(p, CTSC_SK_CloseBraceToken);
    int end = cur_full_start(p);
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_NamedExports, fs, end);
    n->data.namedExports.elements = elements;
    return n;
}

/*
 * Pre: `export` consumed; mirrors parser.ts parseExportDeclaration (~8701).
 */
static CtscNode* parse_export_declaration_after_export_keyword(Parser* p, int export_pos) {
    bool is_type_only = false;
    if (cur(p) == CTSC_SK_TypeKeyword && !p->scanner.current.has_preceding_line_break) {
        advance(p);
        is_type_only = true;
    }

    CtscNode* export_clause = NULL;
    CtscNode* module_specifier = NULL;

    if (cur(p) == CTSC_SK_AsteriskToken) {
        int star_fs = cur_full_start(p);
        advance(p);
        if (cur(p) == CTSC_SK_AsKeyword && !p->scanner.current.has_preceding_line_break) {
            advance(p);
            CtscNode* bind_name = parse_import_specifier_name(p);
            if (!bind_name) return NULL;
            CtscNode* ns = ctsc_node_new(p->arena, CTSC_SK_NamespaceExport, star_fs, bind_name->end);
            ns->data.namespaceExport.name = bind_name;
            export_clause = ns;
        }
        if (!expect(p, CTSC_SK_FromKeyword)) return NULL;
        module_specifier = parse_module_specifier_string(p);
        if (!module_specifier) return NULL;
    } else if (cur(p) == CTSC_SK_OpenBraceToken) {
        export_clause = parse_named_exports(p);
        if (!export_clause) return NULL;
        if (cur(p) == CTSC_SK_FromKeyword
            || (cur(p) == CTSC_SK_StringLiteral && !p->scanner.current.has_preceding_line_break)) {
            if (!expect(p, CTSC_SK_FromKeyword)) return NULL;
            module_specifier = parse_module_specifier_string(p);
            if (!module_specifier) return NULL;
        }
    } else {
        return NULL;
    }

    int decl_end = module_specifier ? module_specifier->end : export_clause->end;
    if (cur(p) == CTSC_SK_SemicolonToken) {
        decl_end = cur_end(p);
        advance(p);
    }

    CtscNode* decl = ctsc_node_new(p->arena, CTSC_SK_ExportDeclaration, export_pos, decl_end);
    decl->data.exportDeclaration.is_type_only = is_type_only;
    decl->data.exportDeclaration.export_clause = export_clause;
    decl->data.exportDeclaration.module_specifier = module_specifier;
    return decl;
}

static CtscNode* parse_statement(Parser* p) {
    int fs = cur_full_start(p);
    switch (cur(p)) {
        case CTSC_SK_SemicolonToken: {
            int end = cur_end(p);
            advance(p);
            return ctsc_node_new(p->arena, CTSC_SK_EmptyStatement, fs, end);
        }
        case CTSC_SK_OpenBraceToken:
            return parse_block(p);
        case CTSC_SK_VarKeyword:
        case CTSC_SK_ConstKeyword:
            return parse_variable_statement(p);
        case CTSC_SK_LetKeyword:
            if (is_let_declaration(p)) return parse_variable_statement(p);
            break;
        case CTSC_SK_FunctionKeyword:
            return parse_function_declaration(p, false);
        case CTSC_SK_AsyncKeyword: {
            /* Mirrors upstream parser.ts parseStatement (~7305): AsyncKeyword may
             * start `async function ...` (parseFunctionDeclaration with AsyncKeyword). */
            int async_fs = cur_full_start(p);
            CtscScanner saved = p->scanner;
            advance(p);
            if (!p->scanner.current.has_preceding_line_break && cur(p) == CTSC_SK_FunctionKeyword) {
                CtscNode* fn = parse_function_declaration(p, true);
                if (fn && fn->kind == CTSC_SK_FunctionDeclaration) {
                    fn->pos = async_fs;
                }
                return fn;
            }
            p->scanner = saved;
            break;
        }
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseStatement
         * (~7445-7457): ExportKeyword routes to parseDeclaration when
         * isStartOfDeclaration() is true. parseDeclarationWorker (~7514-7515)
         * then parses FunctionDeclaration with modifiers containing
         * ExportKeyword. For `export function ...` the declaration span starts
         * at the `export` keyword (finishNode pos). */
        case CTSC_SK_ExportKeyword: {
            int export_pos = cur_full_start(p);
            CtscScanner saved = p->scanner;
            advance(p); /* export */
            bool export_async = false;
            bool is_default_export = false;
            /* Mirrors upstream parser.ts parseDeclaration (~7530-7535):
             * `export default` → parseExportAssignment (~8736); for
             * `export default function ...` the assignment expression is a
             * function, which we model as FunctionDeclaration + has_export_default. */
            if (cur(p) == CTSC_SK_DefaultKeyword && !p->scanner.current.has_preceding_line_break) {
                advance(p); /* default */
                is_default_export = true;
            }
            if (is_default_export) {
                if (cur(p) == CTSC_SK_AsyncKeyword && !p->scanner.current.has_preceding_line_break) {
                    advance(p);
                    export_async = true;
                }
            } else {
                if (cur(p) == CTSC_SK_AsyncKeyword && !p->scanner.current.has_preceding_line_break) {
                    advance(p);
                    export_async = true;
                }
            }
            if (cur(p) == CTSC_SK_FunctionKeyword) {
                CtscNode* fn = parse_function_declaration(p, export_async);
                if (fn && fn->kind == CTSC_SK_FunctionDeclaration) {
                    if (is_default_export) {
                        fn->data.functionDeclaration.has_export_default = true;
                    } else {
                        fn->data.functionDeclaration.has_export = true;
                    }
                    fn->pos = export_pos;
                }
                return fn;
            }
            if (is_default_export) {
                p->scanner = saved;
                break;
            }
            if (cur(p) == CTSC_SK_VarKeyword || cur(p) == CTSC_SK_ConstKeyword
                || (cur(p) == CTSC_SK_LetKeyword && is_let_declaration(p))) {
                CtscNode* stmt = parse_variable_statement(p);
                if (stmt && stmt->kind == CTSC_SK_VariableStatement) {
                    stmt->data.variableStatement.has_export = true;
                    stmt->pos = export_pos;
                }
                return stmt;
            }
            /* Mirrors upstream parser.ts parseDeclaration (~7536): `export` may be
             * followed by TS modifier keywords before ClassKeyword, e.g.
             * `export abstract class C {}` (selfhost-derived 87_export_abstract.ts). */
            {
                CtscScanner saved_before_class_mods = p->scanner;
                CtscNodeArray export_class_mods;
                ctsc_node_array_init(&export_class_mods);
                while (is_ts_modifier_keyword(cur(p)) && !p->scanner.current.has_preceding_line_break) {
                    CtscSyntaxKind mk = cur(p);
                    int mpos = cur_full_start(p);
                    advance(p);
                    int mend = cur_full_start(p);
                    CtscNode* m = ctsc_node_new(p->arena, mk, mpos, mend);
                    ctsc_node_array_push(&export_class_mods, p->arena, m);
                }
                if (cur(p) == CTSC_SK_ClassKeyword) {
                    CtscNode* cls = parse_class_declaration_or_expression_with_modifiers(
                        p, CTSC_SK_ClassDeclaration, export_pos,
                        export_class_mods.len > 0 ? &export_class_mods : NULL);
                    if (cls && cls->kind == CTSC_SK_ClassDeclaration) {
                        cls->data.classDeclaration.has_export = true;
                    }
                    return cls;
                }
                p->scanner = saved_before_class_mods;
            }
            if (cur(p) == CTSC_SK_EnumKeyword) {
                CtscNode* en = parse_enum_declaration(p);
                if (en && en->kind == CTSC_SK_EnumDeclaration) {
                    en->data.enumDeclaration.has_export = true;
                    en->pos = export_pos;
                }
                return en;
            }
            /* Mirrors upstream parser.ts parseDeclaration (~7467): `export interface`
             * (parseInterfaceDeclaration with ExportKeyword in modifiers). */
            if (cur(p) == CTSC_SK_InterfaceKeyword) {
                int iface_kw_pos = cur_full_start(p);
                CtscNodeArray export_mods;
                ctsc_node_array_init(&export_mods);
                CtscNode* ek = ctsc_node_new(p->arena, CTSC_SK_ExportKeyword, export_pos, iface_kw_pos);
                ctsc_node_array_push(&export_mods, p->arena, ek);
                return parse_interface_declaration_with_modifiers(p, export_pos, &export_mods);
            }
            /* `export type { ... } from` is ExportDeclaration, not TypeAliasDeclaration
             * (parser.ts parseExportDeclaration ~8707). */
            if (cur(p) == CTSC_SK_TypeKeyword && !p->scanner.current.has_preceding_line_break) {
                CtscScanner saved_before_type = p->scanner;
                advance(p);
                if (cur(p) == CTSC_SK_OpenBraceToken) {
                    p->scanner = saved_before_type;
                    CtscNode* ed = parse_export_declaration_after_export_keyword(p, export_pos);
                    if (ed) return ed;
                }
                p->scanner = saved_before_type;
                int type_kw_pos = cur_full_start(p);
                CtscNodeArray export_mods;
                ctsc_node_array_init(&export_mods);
                CtscNode* ek = ctsc_node_new(p->arena, CTSC_SK_ExportKeyword, export_pos, type_kw_pos);
                ctsc_node_array_push(&export_mods, p->arena, ek);
                return parse_type_alias_declaration(p, export_pos, &export_mods);
            }
            /* export-from: `export { ... } from`, `export * from`, `export * as ns from` */
            if (cur(p) == CTSC_SK_OpenBraceToken || cur(p) == CTSC_SK_AsteriskToken) {
                CtscNode* ed = parse_export_declaration_after_export_keyword(p, export_pos);
                if (ed) return ed;
            }
            p->scanner = saved;
            break;
        }
        case CTSC_SK_ClassKeyword:
            return parse_class_declaration(p);
        case CTSC_SK_InterfaceKeyword:
            /* Mirrors upstream parser.ts parseStatement (~7519):
             *     case SyntaxKind.InterfaceKeyword:
             *         return parseInterfaceDeclaration(...);
             * routed via parseDeclaration when isStartOfDeclaration is true.
             * ctsc dispatches directly on InterfaceKeyword here — when the
             * following token is not a valid identifier the parser emits a
             * zero-width missing Identifier (same error-recovery as tsc). */
            return parse_interface_declaration(p);
        /* Mirrors upstream parser.ts parseStatement (~7521) → parseDeclaration
         * (~7467) → parseTypeAliasDeclaration (~8249) for `type Name = ...`. */
        case CTSC_SK_TypeKeyword:
            return parse_type_alias_declaration(p, cur_full_start(p), NULL);
        case CTSC_SK_EnumKeyword:
            return parse_enum_declaration(p);
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseStatement
         * (~7525-7527) → parseDeclaration → parseModuleDeclaration: the
         * `module` / `namespace` contextual keywords open a ModuleDeclaration
         * only when isStartOfDeclaration (~7208) is true, which for these
         * keywords reduces to nextTokenIsIdentifierOrStringLiteralOnSameLine
         * (~7562). Otherwise the keyword is an identifier in expression
         * position (e.g. `namespace;` or `namespace + 1`). ctsc has not yet
         * modelled the StringLiteral / GlobalKeyword ambient-module forms,
         * but the simple-identifier dispatch is sufficient for the
         * currently-unlocked fixture (108_parserModuleDeclaration6.ts:
         * `namespace number {}` — `number` is NumberKeyword which
         * is_binding_identifier_kind accepts as an identifier). */
        case CTSC_SK_ModuleKeyword:
        case CTSC_SK_NamespaceKeyword:
            if (next_token_is_identifier_or_string_literal_on_same_line(p)) {
                return parse_module_declaration(p);
            }
            break;
        case CTSC_SK_IfKeyword:
            return parse_if_statement(p);
        case CTSC_SK_WhileKeyword:
            return parse_while_statement(p);
        case CTSC_SK_DoKeyword:
            return parse_do_statement(p);
        case CTSC_SK_ForKeyword:
            return parse_for_statement(p);
        case CTSC_SK_ReturnKeyword:
            return parse_return_statement(p);
        case CTSC_SK_ThrowKeyword:
            return parse_throw_statement(p);
        case CTSC_SK_WithKeyword:
            return parse_with_statement(p);
        case CTSC_SK_SwitchKeyword:
            return parse_switch_statement(p);
        case CTSC_SK_BreakKeyword:
            return parse_break_or_continue_statement(p, CTSC_SK_BreakStatement);
        case CTSC_SK_ContinueKeyword:
            return parse_break_or_continue_statement(p, CTSC_SK_ContinueStatement);
        case CTSC_SK_DebuggerKeyword:
            return parse_debugger_statement(p);
        /* Mirrors upstream parser.ts parseImportDeclarationOrImportEqualsDeclaration
         * (~8384). Unsupported forms fall through to the expression-statement path. */
        case CTSC_SK_ImportKeyword: {
            CtscScanner saved = p->scanner;
            int decl_fs = cur_full_start(p);
            advance(p);
            CtscNode* imp = parse_import_declaration_after_import_keyword(p, decl_fs);
            if (imp) return imp;
            p->scanner = saved;
            break;
        }
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseStatement
         * (~7427-7432): TryKeyword, CatchKeyword, FinallyKeyword all route
         * into parseTryStatement. The latter two are error-recovery entries
         * so a stray `catch` or `finally` at statement start still parses
         * (see the 106_parserMissingToken1.ts fixture `a / finally`). */
        case CTSC_SK_TryKeyword:
        case CTSC_SK_CatchKeyword:
        case CTSC_SK_FinallyKeyword:
            return parse_try_statement(p);
        /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseStatement
         * (~7442) → parseDeclaration (~7467): `declare` is a TS-only modifier
         * that, followed by a declaration-starter on the same line (see
         * isDeclaration ~7214 DeclareKeyword branch + isStartOfDeclaration
         * ~7264), routes into parseDeclaration and the declaration inherits
         * ModifierFlags.Ambient. The TS-to-JS transformer elides every such
         * statement via transformers/ts.ts visitTypeScript (~643:
         * `hasSyntacticModifier(node, ModifierFlags.Ambient)` →
         * createNotEmittedStatement). ctsc currently models that elision by
         * tagging the VariableStatement with `has_declare` and letting the
         * emitter drop it through source_file_statement_is_dropped().
         *
         * Only the var/let/const declaration keywords that ctsc already
         * parses are covered here (fixture 107_parserSkippedTokens19.ts:
         * `\ declare var v;`). When a `declare function` / `declare class`
         * fixture unlocks, extend the lookahead and propagate has_declare
         * onto those statement data structs. */
        case CTSC_SK_DeclareKeyword: {
            CtscScanner saved = p->scanner;
            advance(p); /* consume `declare` speculatively */
            /* parser.ts isDeclaration ~7222: ASI kicks in when a line break
             * separates `declare` from the next token, aborting the
             * declaration attempt. */
            if (!p->scanner.current.has_preceding_line_break) {
                if (cur(p) == CTSC_SK_VarKeyword
                    || cur(p) == CTSC_SK_ConstKeyword
                    || (cur(p) == CTSC_SK_LetKeyword && is_let_declaration(p))) {
                    CtscNode* stmt = parse_variable_statement(p);
                    if (stmt && stmt->kind == CTSC_SK_VariableStatement) {
                        stmt->data.variableStatement.has_declare = true;
                        /* parser.ts finishNode (~2600) positions the node at the
                         * pre-modifier getNodePos(); mirror that here so the
                         * VariableStatement span covers the `declare` keyword. */
                        stmt->pos = fs;
                    }
                    return stmt;
                }
                if (cur(p) == CTSC_SK_FunctionKeyword) {
                    CtscNode* fn = parse_function_declaration(p, false);
                    if (fn && fn->kind == CTSC_SK_FunctionDeclaration) {
                        fn->data.functionDeclaration.has_declare = true;
                        fn->pos = fs;
                    }
                    return fn;
                }
            }
            /* Not a declaration we can parse; fall back to expression-statement
             * treatment of `declare` as a contextual identifier. */
            p->scanner = saved;
            break;
        }
        /* Mirrors upstream parser.ts parseStatement (~7447-7457): a TS-only
         * accessibility/modifier keyword in statement position routes into
         * parseDeclaration only when isStartOfDeclaration() is true. When it
         * isn't (and the next token on the same line is an identifier /
         * keyword, which is the only case in which parseList would reject
         * it — isStartOfStatement ~7324), parseList dispatches to
         * abortParsingListOrMoveToNextToken (~3410): report
         * Diagnostics.Declaration_or_statement_expected (code 1128) at the
         * modifier, consume exactly one token, and continue the loop so the
         * FOLLOWING token starts a new statement attempt. Fixture
         * 107_parserPublicBreak1.ts (`public break;`) exercises this: tsc
         * produces a single BreakStatement at the `break` keyword (no
         * ExpressionStatement for `public`) with a 1128 error at pos 19
         * length 6. ctsc has not yet grown parseDeclaration with modifiers,
         * so the declaration arm is unreachable here; any fixture that
         * requires it will add its own case. */
        case CTSC_SK_PublicKeyword:
        case CTSC_SK_PrivateKeyword:
        case CTSC_SK_ProtectedKeyword:
        case CTSC_SK_StaticKeyword:
        case CTSC_SK_ReadonlyKeyword:
        /* Mirrors upstream parser.ts parseStatement (~7450): AbstractKeyword
         * is one of the TS modifier keywords that routes into parseDeclaration
         * when isStartOfDeclaration returns true. For
         * 108_classAbstractWithInterface.ts (`abstract interface I {}`) the
         * peek past the contiguous modifier run lands on InterfaceKeyword,
         * dispatching into parse_interface_declaration_with_modifiers below. */
        case CTSC_SK_AbstractKeyword: {
            /* Mirrors upstream parser.ts parseStatement (~7447-7459) →
             * parseDeclaration (~7467): a TS-only modifier routes into
             * parseModifiers + parseDeclarationWorker when isStartOfDeclaration
             * (~7264) is true. isStartOfDeclaration lookahead (~7211) consumes
             * the modifier and recurses on the following token; for
             * `protected class ...` that resolves true via the ClassKeyword
             * arm (returns true at isDeclaration ~7186). ctsc short-circuits
             * the lookahead by peeking past the contiguous modifier run and
             * dispatching on the first non-modifier token directly.
             *
             * Declaration arms currently covered: ClassDeclaration
             * (fixtures/parser/from-upstream/108_Protected1.ts:
             * `protected class C {}`) and InterfaceDeclaration
             * (108_classAbstractWithInterface.ts: `abstract interface I {}`).
             * Other declaration kinds (function / enum / module /
             * var-let-const) will unlock as fixtures bring them in. When no
             * declaration follows, fall through to the pre-existing 1128
             * recovery path that matches upstream's
             * abortParsingListOrMoveToNextToken (~3410) for fixtures like
             * 107_parserPublicBreak1.ts (`public break;`). */
            CtscScanner saved = p->scanner;
            /* Peek: consume contiguous modifier keywords and look at what
             * follows without producing any nodes yet. parser.ts
             * canFollowModifier (~7222) requires no line break between a
             * modifier and the next token — ASI otherwise turns the
             * modifier into its own ExpressionStatement. */
            bool same_line = true;
            while (is_ts_modifier_keyword(cur(p))) {
                advance(p);
                if (p->scanner.current.has_preceding_line_break) {
                    same_line = false;
                    break;
                }
            }
            CtscSyntaxKind after = cur(p);
            p->scanner = saved;
            if (same_line && after == CTSC_SK_ClassKeyword) {
                int pos = cur_full_start(p);
                CtscNodeArray modifiers; ctsc_node_array_init(&modifiers);
                parse_modifiers(p, &modifiers);
                return parse_class_declaration_or_expression_with_modifiers(
                    p, CTSC_SK_ClassDeclaration, pos, &modifiers);
            }
            if (same_line && after == CTSC_SK_InterfaceKeyword) {
                int pos = cur_full_start(p);
                CtscNodeArray modifiers; ctsc_node_array_init(&modifiers);
                parse_modifiers(p, &modifiers);
                return parse_interface_declaration_with_modifiers(p, pos, &modifiers);
            }
            if (is_ts_modifier_keyword(cur(p))
                && next_token_is_identifier_or_keyword_on_same_line(p)) {
                ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1128,
                    cur_start(p), cur_end(p) - cur_start(p),
                    "Declaration or statement expected.");
                advance(p);
                return NULL;
            }
            break;
        }
        default: break;
    }
    return parse_expression_or_labeled_statement(p, fs);
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseExpressionOrLabeledStatement (~7123).
 */
static CtscNode* parse_expression_or_labeled_statement(Parser* p, int stmt_start) {
    CtscNode* expression = parse_expression(p);
    if (!expression) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 9000, cur_start(p), cur_end(p) - cur_start(p),
            "ctsc parser: unrecognized statement start (token %s).", ctsc_syntax_kind_name(cur(p)));
        advance(p);
        return NULL;
    }
    if (expression->kind == CTSC_SK_Identifier && cur(p) == CTSC_SK_ColonToken) {
        advance(p);
        CtscNode* body = parse_statement(p);
        int end = body ? body->end : expression->end;
        CtscNode* lab = ctsc_node_new(p->arena, CTSC_SK_LabeledStatement, stmt_start, end);
        lab->data.labeledStatement.label = expression;
        lab->data.labeledStatement.statement = body;
        return lab;
    }
    int end = expression->end;
    if (cur(p) == CTSC_SK_SemicolonToken) { end = cur_end(p); advance(p); }
    CtscNode* stmt = ctsc_node_new(p->arena, CTSC_SK_ExpressionStatement, stmt_start, end);
    stmt->data.expressionStatement.expression = expression;
    return stmt;
}

CtscParseResult ctsc_parse(const char* src, size_t len, CtscArena* arena) {
    CtscParseResult r;
    r.arena = arena;
    r.diagnostics = (CtscDiagnosticList*)ctsc_arena_alloc(arena, sizeof(CtscDiagnosticList));
    ctsc_diag_init(r.diagnostics);

    Parser p;
    p.arena = arena;
    p.diagnostics = r.diagnostics;
    ctsc_scanner_init(&p.scanner, src, len, arena, r.diagnostics);
    p.source_end = (int)p.scanner.source.len;
    p.await_context_depth = 0;

    CtscNode* sf = ctsc_node_new(arena, CTSC_SK_SourceFile, 0, p.source_end);
    ctsc_node_array_init(&sf->data.sourceFile.statements);

    advance(&p);
    while (cur(&p) != CTSC_SK_EndOfFileToken) {
        int before = (int)p.scanner.pos;
        CtscNode* stmt = parse_statement(&p);
        if (stmt) ctsc_node_array_push(&sf->data.sourceFile.statements, arena, stmt);
        if ((int)p.scanner.pos == before) advance(&p);
    }
    /* cur is EndOfFileToken: its full_start is scanner.getTokenFullStart(),
     * which tsc assigns to the surrounding statements NodeArray .end. */
    sf->data.sourceFile.statements_end = cur_full_start(&p);

    r.sourceFile = sf;
    return r;
}
