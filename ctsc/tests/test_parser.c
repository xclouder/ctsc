#include "ctsc/parser.h"
#include "ctsc/arena.h"
#include "ctsc/ast.h"
#include <stdio.h>
#include <string.h>

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)

int test_parser(void) {
    int failed = 0;
    CtscArena a; ctsc_arena_init(&a, 4096);
    CtscParseResult r = ctsc_parse("", 0, &a);
    EXPECT(r.sourceFile != NULL);
    EXPECT(r.sourceFile->kind == CTSC_SK_SourceFile);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 0);
    ctsc_arena_free(&a);

    ctsc_arena_init(&a, 4096);
    const char* src = ";";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    EXPECT(r.sourceFile->data.sourceFile.statements.items[0]->kind == CTSC_SK_EmptyStatement);
    ctsc_arena_free(&a);

    ctsc_arena_init(&a, 4096);
    src = "42;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    const CtscNode* s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    EXPECT(s->data.expressionStatement.expression != NULL);
    EXPECT(s->data.expressionStatement.expression->kind == CTSC_SK_NumericLiteral);
    ctsc_arena_free(&a);

    /* Mirrors upstream VariableDeclaration6_es6: lone `let` with no binding is an expression
     * (parser.ts parseStatement LetKeyword + isLetDeclaration, ~7388–7391). */
    ctsc_arena_init(&a, 4096);
    src = "// @target:es6\nlet";
    int src_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)src_end, &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    EXPECT(s->data.expressionStatement.expression->kind == CTSC_SK_Identifier);
    EXPECT(s->pos == 0 && s->end == src_end);
    EXPECT(s->data.expressionStatement.expression->pos == 0 && s->data.expressionStatement.expression->end == src_end);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseUpdateExpression (~5875): PostfixUnaryExpression
     * wraps an LHS followed by '++' on the same line. Exercise the
     * parserPostfixPostfixExpression1 shape: `a++ ++;` -> two statements, the
     * first an ExpressionStatement whose expression is a PostfixUnaryExpression. */
    ctsc_arena_init(&a, 4096);
    src = "a++;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    EXPECT(s->data.expressionStatement.expression->kind == CTSC_SK_PostfixUnaryExpression);
    EXPECT(s->data.expressionStatement.expression->data.postfixUnaryExpression.operator_kind == CTSC_SK_PlusPlusToken);
    EXPECT(s->data.expressionStatement.expression->data.postfixUnaryExpression.operand->kind == CTSC_SK_Identifier);
    /* expression.end equals scanner.getTokenFullStart() AFTER '++' = start of ';' = 3. */
    EXPECT(s->data.expressionStatement.expression->end == 3);
    ctsc_arena_free(&a);

    /* Prefix '++' on a missing operand: `++;` should produce a PrefixUnaryExpression
     * whose operand is a zero-width missing Identifier (pos == end, empty text).
     * Mirrors parser.ts createMissingNode<Identifier> (~2619). */
    ctsc_arena_init(&a, 4096);
    src = "++;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* pu = s->data.expressionStatement.expression;
    EXPECT(pu->kind == CTSC_SK_PrefixUnaryExpression);
    EXPECT(pu->data.prefixUnaryExpression.operand->kind == CTSC_SK_Identifier);
    EXPECT(pu->data.prefixUnaryExpression.operand->pos == pu->data.prefixUnaryExpression.operand->end);
    EXPECT(pu->data.prefixUnaryExpression.operand->data.identifier.text_len == 0);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parsePrimaryExpression (~6619): ThisKeyword,
     * SuperKeyword, NullKeyword, TrueKeyword, FalseKeyword are emitted as
     * token-only leaf nodes via parseTokenNode<PrimaryExpression>(), keeping
     * their keyword SyntaxKind (NOT Identifier). Exercise via `++this;` from
     * parserUnaryExpression1.ts. */
    ctsc_arena_init(&a, 4096);
    src = "++this;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* pu2 = s->data.expressionStatement.expression;
    EXPECT(pu2->kind == CTSC_SK_PrefixUnaryExpression);
    EXPECT(pu2->data.prefixUnaryExpression.operand->kind == CTSC_SK_ThisKeyword);
    EXPECT(pu2->data.prefixUnaryExpression.operand->pos == 2);
    EXPECT(pu2->data.prefixUnaryExpression.operand->end == 6);
    ctsc_arena_free(&a);

    /* true / false / null as standalone primary expressions. */
    ctsc_arena_init(&a, 4096);
    src = "true;";
    r = ctsc_parse(src, strlen(src), &a);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->data.expressionStatement.expression->kind == CTSC_SK_TrueKeyword);
    ctsc_arena_free(&a);

    ctsc_arena_init(&a, 4096);
    src = "false;";
    r = ctsc_parse(src, strlen(src), &a);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->data.expressionStatement.expression->kind == CTSC_SK_FalseKeyword);
    ctsc_arena_free(&a);

    ctsc_arena_init(&a, 4096);
    src = "null;";
    r = ctsc_parse(src, strlen(src), &a);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->data.expressionStatement.expression->kind == CTSC_SK_NullKeyword);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseNewExpressionOrNewDotTarget (~6801):
     * `new Date` builds a NewExpression whose expression is the member chain
     * and whose arguments NodeArray is absent (tsc's `undefined`). Covers the
     * 105_parserConstructorAmbiguity2.ts parity shape where `new Date<A` has
     * the type-argument try-parse rolled back, leaving just the identifier. */
    ctsc_arena_init(&a, 4096);
    src = "new Date";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* ne = s->data.expressionStatement.expression;
    EXPECT(ne->kind == CTSC_SK_NewExpression);
    EXPECT(ne->pos == 0);
    EXPECT(ne->end == 8);
    EXPECT(ne->data.newExpression.has_arguments == false);
    EXPECT(ne->data.newExpression.expression != NULL);
    EXPECT(ne->data.newExpression.expression->kind == CTSC_SK_Identifier);
    EXPECT(ne->data.newExpression.expression->pos == 3);
    EXPECT(ne->data.newExpression.expression->end == 8);
    ctsc_arena_free(&a);

    /* `new D()` carries an empty arguments NodeArray (tsc's arguments.length === 0
     * with arguments !== undefined). Exercises has_arguments=true and the
     * CloseParenToken end handling. */
    ctsc_arena_init(&a, 4096);
    src = "new D();";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* ne2 = s->data.expressionStatement.expression;
    EXPECT(ne2->kind == CTSC_SK_NewExpression);
    EXPECT(ne2->data.newExpression.has_arguments == true);
    EXPECT(ne2->data.newExpression.arguments.len == 0);
    EXPECT(ne2->end == 7);
    ctsc_arena_free(&a);

    return failed;
}
