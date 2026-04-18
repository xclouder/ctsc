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
    n->data.identifier.text = p->scanner.current.text;
    n->data.identifier.text_len = p->scanner.current.text_len;
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

static CtscNode* parse_expression(Parser* p);
static CtscNode* parse_assignment_expression(Parser* p);
static bool token_is_identifier_expression(CtscSyntaxKind k);
static bool is_let_declaration(Parser* p);
static CtscNode* parse_new_expression(Parser* p);
static CtscNode* make_missing_identifier(Parser* p);
static bool is_yield_expression(Parser* p);
static CtscNode* parse_yield_expression(Parser* p);

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
         * tokenValue for those, matching tsc. */
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
        advance(p);
        return n;
    }
    if (k == CTSC_SK_OpenParenToken) {
        int end_guess = 0;
        advance(p);
        CtscNode* e = parse_expression(p);
        int close_end = cur_end(p);
        expect(p, CTSC_SK_CloseParenToken);
        (void)end_guess;
        CtscNode* paren = ctsc_node_new(p->arena, CTSC_SK_ParenthesizedExpression, fs, close_end);
        paren->data.parenthesizedExpression.expression = e;
        return paren;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseArrayLiteralExpression
     * (~6689): "[" ArgumentOrArrayLiteralElement (, ArgumentOrArrayLiteralElement)* "]".
     * tsc's element parser falls through to parseAssignmentExpressionOrHigher for
     * ordinary cases; ctsc approximates that via parse_assignment_expression.
     * OmittedExpression (consecutive commas) and SpreadElement (...) are not yet
     * modelled in ctsc; they will be grown when fixtures demand them.
     * finishNode's end = scanner.getTokenFullStart() of the token AFTER "]"
     * (== cur_full_start after consuming the CloseBracketToken). */
    if (k == CTSC_SK_OpenBracketToken) {
        advance(p); /* "[" */
        CtscNodeArray elements; ctsc_node_array_init(&elements);
        if (cur(p) != CTSC_SK_CloseBracketToken) {
            for (;;) {
                /* Skip over (not model) a leading comma -> OmittedExpression
                 * placeholder; see parser.ts parseArgumentOrArrayLiteralElement
                 * (~6679). Stop if we hit "]" or EOF. */
                if (cur(p) == CTSC_SK_CommaToken) { advance(p); continue; }
                CtscNode* e = parse_assignment_expression(p);
                if (!e) break;
                ctsc_node_array_push(&elements, p->arena, e);
                if (!accept(p, CTSC_SK_CommaToken)) break;
            }
        }
        expect(p, CTSC_SK_CloseBracketToken);
        int end = cur_full_start(p);
        CtscNode* arr = ctsc_node_new(p->arena, CTSC_SK_ArrayLiteralExpression, fs, end);
        arr->data.arrayLiteralExpression.elements = elements;
        return arr;
    }
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectLiteralExpression
     * (~6755): "{" ObjectLiteralElement (, ObjectLiteralElement)* "}".
     * ObjectLiteralElement parsing is not yet implemented in ctsc; we consume
     * tokens between matched braces so the outer expression shape (pos/end) is
     * correct. The oracle (harness/src/oracle-ast.ts) serializes this kind via
     * the default branch which only emits kind/pos/end, so no properties field
     * is required for parity. */
    if (k == CTSC_SK_OpenBraceToken) {
        advance(p); /* "{" */
        CtscNodeArray properties; ctsc_node_array_init(&properties);
        int depth = 1;
        while (depth > 0 && cur(p) != CTSC_SK_EndOfFileToken) {
            if (cur(p) == CTSC_SK_OpenBraceToken) { depth++; advance(p); continue; }
            if (cur(p) == CTSC_SK_CloseBraceToken) { depth--; if (depth == 0) break; advance(p); continue; }
            advance(p);
        }
        int close_end = cur_end(p);
        expect(p, CTSC_SK_CloseBraceToken);
        CtscNode* obj = ctsc_node_new(p->arena, CTSC_SK_ObjectLiteralExpression, fs, close_end);
        obj->data.objectLiteralExpression.properties = properties;
        return obj;
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
static CtscNode* parse_element_access_rest_after_open(Parser* p, CtscNode* left) {
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
    return ea;
}

static CtscNode* parse_call_or_property_rest(Parser* p, CtscNode* left) {
    for (;;) {
        if (cur(p) == CTSC_SK_OpenParenToken) {
            int fs = left->pos;
            advance(p);
            CtscNodeArray args; ctsc_node_array_init(&args);
            if (cur(p) != CTSC_SK_CloseParenToken) {
                for (;;) {
                    CtscNode* a = parse_expression(p);
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
        if (cur(p) == CTSC_SK_DotToken) {
            advance(p);
            CtscNode* name = parse_identifier(p);
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
            left = parse_element_access_rest_after_open(p, left);
            continue;
        }
        break;
    }
    return left;
}

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseMemberExpressionRest
 * (~6453) restricted to the non-tagged-template / non-type-arguments subset
 * currently modelled in ctsc: property access (`.name`) and element access
 * (`[argument]`). Unlike parse_call_or_property_rest, this helper
 * intentionally stops at the first `(` so that `new X(args)` builds a
 * NewExpression (arguments consumed by parse_new_expression below) rather
 * than CallExpression(X, args). Optional chains (`?.`) are not yet modelled;
 * they will be grown when fixtures demand them.
 */
static CtscNode* parse_member_expression_rest(Parser* p, CtscNode* left) {
    for (;;) {
        if (cur(p) == CTSC_SK_DotToken) {
            advance(p);
            CtscNode* name = parse_identifier(p);
            if (!name) break;
            CtscNode* pa = ctsc_node_new(p->arena, CTSC_SK_PropertyAccessExpression, left->pos, name->end);
            pa->data.propertyAccessExpression.expression = left;
            pa->data.propertyAccessExpression.name = name;
            left = pa;
            continue;
        }
        if (cur(p) == CTSC_SK_OpenBracketToken) {
            advance(p);
            left = parse_element_access_rest_after_open(p, left);
            continue;
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
 * ctsc does not yet model MetaProperty (`new.target`) or absorb
 * ExpressionWithTypeArguments; tsc rolls the type-arguments try-parse back
 * when `<...` fails to close (see 105_parserConstructorAmbiguity2.ts), so a
 * straight `new <MemberExpression>` with optional `(Arguments)` matches.
 *
 * finishNode end (~2600): scanner.getTokenFullStart() of the next token.
 *   - Without arguments: expression.end (property-access rest terminates at
 *     the token full_start before the next non-trivia token).
 *   - With arguments: cur_full_start AFTER consuming `)`, == tsc's CallArguments
 *     finishNode end.
 */
static CtscNode* parse_new_expression(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* `new` */
    CtscNode* expression = parse_primary(p);
    if (!expression) {
        /* Mirrors parser.ts parsePrimaryExpression (~6660) default branch when
         * no primary is available: synthesize a zero-width missing Identifier. */
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
            cur_start(p), cur_end(p) - cur_start(p),
            "Expression expected.");
        expression = make_missing_identifier(p);
    } else {
        expression = parse_member_expression_rest(p, expression);
    }
    CtscNode* n = ctsc_node_new(p->arena, CTSC_SK_NewExpression, fs, expression->end);
    n->data.newExpression.expression = expression;
    n->data.newExpression.has_arguments = false;
    ctsc_node_array_init(&n->data.newExpression.arguments);
    if (cur(p) == CTSC_SK_OpenParenToken) {
        advance(p);
        if (cur(p) != CTSC_SK_CloseParenToken) {
            for (;;) {
                CtscNode* a = parse_assignment_expression(p);
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
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseVoidExpression (~5708)
     * via parseSimpleUnaryExpression's case SyntaxKind.VoidKeyword (~5807):
     *   `void <UnaryExpression>`.
     * finishNode end defaults to scanner.getTokenFullStart() AFTER the operand
     * is parsed, which equals operand->end here (ctsc's finishNode convention
     * for prefix-unary; see PrefixUnaryExpression above). */
    if (cur(p) == CTSC_SK_VoidKeyword) {
        int fs = cur_full_start(p);
        advance(p); /* void */
        CtscNode* operand = parse_unary(p);
        if (!operand) {
            /* Same recovery as simple prefix unary above: parsePrimaryExpression
             * (~6660) bottoms out at parseIdentifier(Expression_expected),
             * synthesizing a zero-width missing Identifier so the VoidExpression
             * still has a well-defined operand.end. */
            ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 1109,
                cur_start(p), cur_end(p) - cur_start(p),
                "Expression expected.");
            operand = make_missing_identifier(p);
        }
        CtscNode* v = ctsc_node_new(p->arena, CTSC_SK_VoidExpression, fs, operand->end);
        v->data.voidExpression.expression = operand;
        return v;
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
    return k == CTSC_SK_LessThanToken || k == CTSC_SK_GreaterThanToken
        || k == CTSC_SK_LessThanEqualsToken || k == CTSC_SK_GreaterThanEqualsToken;
}
static bool is_logical_and_op(CtscSyntaxKind k) { return k == CTSC_SK_AmpersandAmpersandToken; }
static bool is_logical_or_op(CtscSyntaxKind k)  { return k == CTSC_SK_BarBarToken; }

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
 */
static CtscNode* parse_binary_level(Parser* p, CtscNode* (*down)(Parser*), bool (*is_op)(CtscSyntaxKind)) {
    CtscNode* left = down(p);
    if (!left) return NULL;
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
static CtscNode* parse_relational(Parser* p)     { return parse_binary_level(p, parse_shift,          is_relational_op); }
static CtscNode* parse_equality(Parser* p)       { return parse_binary_level(p, parse_relational,     is_equality_op); }
static CtscNode* parse_logical_and(Parser* p)    { return parse_binary_level(p, parse_equality,       is_logical_and_op); }
static CtscNode* parse_logical_or(Parser* p)     { return parse_binary_level(p, parse_logical_and,    is_logical_or_op); }

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
 * Kinds ctsc does not yet model (JsxElement, TaggedTemplateExpression,
 * NonNullExpression, ExpressionWithTypeArguments, MetaProperty,
 * MissingDeclaration, PrivateIdentifier, BigIntLiteral) are omitted for
 * now; they will be grown alongside the parser productions that create
 * them.
 */
static bool is_left_hand_side_expression_kind(CtscSyntaxKind k) {
    switch (k) {
        case CTSC_SK_PropertyAccessExpression:
        case CTSC_SK_ElementAccessExpression:
        case CTSC_SK_NewExpression:
        case CTSC_SK_CallExpression:
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
 */
static CtscNode* parse_assignment_expression(Parser* p) {
    /* Mirrors parser.ts parseAssignmentExpressionOrHigher (~5082): YieldExpression
     * must be checked before any LHS / binary productions so `yield foo` is not
     * parsed as an Identifier expression followed by a second statement. */
    if (is_yield_expression(p)) {
        return parse_yield_expression(p);
    }
    CtscNode* left = parse_conditional(p);
    if (!left) return NULL;
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

static CtscNode* parse_expression(Parser* p) {
    return parse_assignment_expression(p);
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
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeAnnotation (~4961):
 *     TypeAnnotation: ":" Type
 * Returns NULL when no ":" is present. ctsc currently models only the subset
 * of Type productions that appears in the fixtures (TypeLiteral "{...}"); all
 * other shapes are token-skipped to keep VariableDeclaration.end aligned with
 * tsc's finishNode position. This is safe because the oracle omits the
 * declaration's `type` field (harness/src/oracle-ast.ts), so only positions
 * matter for parity.
 */
static CtscNode* parse_type_annotation(Parser* p) {
    if (cur(p) != CTSC_SK_ColonToken) return NULL;
    advance(p); /* ":" */
    if (cur(p) == CTSC_SK_OpenBraceToken) {
        return parse_type_literal(p);
    }
    /* Fallback: consume a minimal "type expression" by scanning a stop set.
     * We advance until we hit a token that clearly terminates a type in the
     * contexts we currently support (VariableDeclaration): `=` (initializer),
     * `,` (next declaration), `;` (statement terminator), `)` / `]` (enclosing
     * paren/bracket), EOF, or a preceding line break (ASI). Nested `{`/`[`
     * are balance-tracked so inline inner braces don't terminate early.
     * This is a conservative placeholder until ctsc grows a real type parser. */
    int fs = cur_full_start(p);
    int brace_depth = 0;
    int bracket_depth = 0;
    int paren_depth = 0;
    int angle_depth = 0;
    while (cur(p) != CTSC_SK_EndOfFileToken) {
        CtscSyntaxKind k = cur(p);
        if (brace_depth == 0 && bracket_depth == 0 && paren_depth == 0 && angle_depth == 0) {
            if (k == CTSC_SK_EqualsToken || k == CTSC_SK_CommaToken
                || k == CTSC_SK_SemicolonToken
                || k == CTSC_SK_CloseParenToken || k == CTSC_SK_CloseBracketToken
                || k == CTSC_SK_CloseBraceToken) {
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
    int end = cur_full_start(p);
    return ctsc_node_new(p->arena, CTSC_SK_TypeReference, fs, end);
}

static CtscNode* parse_variable_declaration(Parser* p) {
    int fs = cur_full_start(p);
    CtscNode* name = parse_identifier(p);
    if (!name) return NULL;
    int end = name->end;
    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseVariableDeclaration
     * (~7649): name (exclamation?) (typeAnnotation?) (initializer?). Skipping
     * the optional exclamation-for-definite-assignment for now; no current
     * fixture exercises `var x!: T`. */
    CtscNode* type = parse_type_annotation(p);
    if (type) end = type->end;
    CtscNode* init = NULL;
    if (accept(p, CTSC_SK_EqualsToken)) {
        init = parse_expression(p);
        if (init) end = init->end;
    }
    CtscNode* decl = ctsc_node_new(p->arena, CTSC_SK_VariableDeclaration, fs, end);
    decl->data.variableDeclaration.name = name;
    decl->data.variableDeclaration.type = type;
    decl->data.variableDeclaration.initializer = init;
    return decl;
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
    for (;;) {
        CtscNode* d = parse_variable_declaration(p);
        if (!d) break;
        ctsc_node_array_push(&decls, p->arena, d);
        if (!accept(p, CTSC_SK_CommaToken)) break;
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

static CtscNode* parse_block(Parser* p) {
    int fs = cur_full_start(p);
    expect(p, CTSC_SK_OpenBraceToken);
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
    return b;
}

static CtscNode* parse_parameter(Parser* p) {
    int fs = cur_full_start(p);
    CtscNode* name = parse_identifier(p);
    if (!name) return NULL;
    CtscNode* pm = ctsc_node_new(p->arena, CTSC_SK_Parameter, fs, name->end);
    pm->data.parameter.name = name;
    return pm;
}

static CtscNode* parse_function_declaration(Parser* p) {
    int fs = cur_full_start(p);
    advance(p); /* function */
    CtscNode* name = NULL;
    if (cur(p) == CTSC_SK_Identifier) name = parse_identifier(p);
    expect(p, CTSC_SK_OpenParenToken);
    CtscNodeArray params; ctsc_node_array_init(&params);
    if (cur(p) != CTSC_SK_CloseParenToken) {
        for (;;) {
            CtscNode* pm = parse_parameter(p);
            if (!pm) break;
            ctsc_node_array_push(&params, p->arena, pm);
            if (!accept(p, CTSC_SK_CommaToken)) break;
        }
    }
    expect(p, CTSC_SK_CloseParenToken);
    CtscNode* body = parse_block(p);
    CtscNode* fn = ctsc_node_new(p->arena, CTSC_SK_FunctionDeclaration, fs, body ? body->end : cur_end(p));
    fn->data.functionDeclaration.name = name;
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
        for (;;) {
            CtscNode* d = parse_variable_declaration(p);
            if (!d) break;
            ctsc_node_array_push(&decls, p->arena, d);
            if (!accept(p, CTSC_SK_CommaToken)) break;
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
            return parse_function_declaration(p);
        case CTSC_SK_IfKeyword:
            return parse_if_statement(p);
        case CTSC_SK_WhileKeyword:
            return parse_while_statement(p);
        case CTSC_SK_ForKeyword:
            return parse_for_statement(p);
        case CTSC_SK_ReturnKeyword:
            return parse_return_statement(p);
        case CTSC_SK_BreakKeyword:
            return parse_break_or_continue_statement(p, CTSC_SK_BreakStatement);
        case CTSC_SK_ContinueKeyword:
            return parse_break_or_continue_statement(p, CTSC_SK_ContinueStatement);
        case CTSC_SK_DebuggerKeyword:
            return parse_debugger_statement(p);
        default: break;
    }
    /* Expression statement */
    CtscNode* expr = parse_expression(p);
    if (!expr) {
        ctsc_diag_push(p->diagnostics, CTSC_DIAG_ERROR, 9000, cur_start(p), cur_end(p) - cur_start(p),
            "ctsc parser: unrecognized statement start (token %s).", ctsc_syntax_kind_name(cur(p)));
        advance(p);
        return NULL;
    }
    int end = expr->end;
    if (cur(p) == CTSC_SK_SemicolonToken) { end = cur_end(p); advance(p); }
    CtscNode* stmt = ctsc_node_new(p->arena, CTSC_SK_ExpressionStatement, fs, end);
    stmt->data.expressionStatement.expression = expr;
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
