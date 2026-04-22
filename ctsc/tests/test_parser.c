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

    /* Mirrors upstream parser.ts parseObjectLiteralElement (~6699) +
     * parseMethodDeclaration (~7782) for the 106_FunctionPropertyAssignments3_es6.ts
     * shape `var v = { *{ } }`: a generator method with a missing name and a
     * missing `(` — parseParameters issues a diagnostic without consuming the
     * next token and parseFunctionBlockOrSemicolon parses the Block as-is.
     * Exercises the MethodDeclaration AST carrying asteriskToken/name/body
     * positions aligned with tsc's finishNode (end = getTokenFullStart of the
     * token AFTER the body). */
    ctsc_arena_init(&a, 4096);
    src = "var v = { *{ } }";
    int src_len = (int)strlen(src);
    r = ctsc_parse(src, (size_t)src_len, &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_VariableStatement);
    const CtscNode* decl = s->data.variableStatement.declarationList
        ->data.variableDeclarationList.declarations.items[0];
    EXPECT(decl->kind == CTSC_SK_VariableDeclaration);
    const CtscNode* obj = decl->data.variableDeclaration.initializer;
    EXPECT(obj != NULL);
    EXPECT(obj->kind == CTSC_SK_ObjectLiteralExpression);
    /* pos is the full_start of `{` which includes the leading space after `=`. */
    EXPECT(obj->pos == 7);
    EXPECT(obj->end == src_len);
    EXPECT(obj->data.objectLiteralExpression.properties.len == 1);
    const CtscNode* md = obj->data.objectLiteralExpression.properties.items[0];
    EXPECT(md->kind == CTSC_SK_MethodDeclaration);
    EXPECT(md->data.methodDeclaration.has_asterisk == true);
    /* asterisk_pos = full_start of `*` (space after `{` is leading trivia). */
    EXPECT(md->data.methodDeclaration.asterisk_pos == 9);
    /* asterisk_end = scanner.getTokenFullStart() of the token after `*`, i.e.
     * full_start of the next `{` (no trivia between them) = 11. */
    EXPECT(md->data.methodDeclaration.asterisk_end == 11);
    const CtscNode* mname = md->data.methodDeclaration.name;
    EXPECT(mname != NULL);
    EXPECT(mname->kind == CTSC_SK_Identifier);
    /* Missing identifier is zero-width at scanner.getTokenFullStart() which is
     * the start of the next `{` (= 11). */
    EXPECT(mname->pos == 11);
    EXPECT(mname->end == 11);
    EXPECT(mname->data.identifier.text_len == 0);
    const CtscNode* body = md->data.methodDeclaration.body;
    EXPECT(body != NULL);
    EXPECT(body->kind == CTSC_SK_Block);
    EXPECT(body->pos == 11);
    EXPECT(body->data.block.statements.len == 0);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseObjectLiteralElement (~6725) +
     * parseMethodDeclaration (~7782) for the 107_FunctionPropertyAssignments6_es6.ts
     * shape `var v = { *<T>() { } }`: a generator method with a missing name
     * (route taken because `*` preceded), then TypeParameters `<T>`, then an
     * empty `()` Parameters, then a Block body. Exercises
     * parse_type_parameters growing the MethodDeclaration children between
     * the name and parameters. */
    ctsc_arena_init(&a, 4096);
    src = "var v = { *<T>() { } }";
    src_len = (int)strlen(src);
    r = ctsc_parse(src, (size_t)src_len, &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_VariableStatement);
    decl = s->data.variableStatement.declarationList
        ->data.variableDeclarationList.declarations.items[0];
    obj = decl->data.variableDeclaration.initializer;
    EXPECT(obj != NULL);
    EXPECT(obj->kind == CTSC_SK_ObjectLiteralExpression);
    EXPECT(obj->data.objectLiteralExpression.properties.len == 1);
    md = obj->data.objectLiteralExpression.properties.items[0];
    EXPECT(md->kind == CTSC_SK_MethodDeclaration);
    EXPECT(md->data.methodDeclaration.has_asterisk == true);
    EXPECT(md->data.methodDeclaration.type_parameters.len == 1);
    {
        const CtscNode* tp = md->data.methodDeclaration.type_parameters.items[0];
        EXPECT(tp->kind == CTSC_SK_TypeParameter);
        EXPECT(tp->data.typeParameter.name != NULL);
        EXPECT(tp->data.typeParameter.name->kind == CTSC_SK_Identifier);
        EXPECT(tp->data.typeParameter.name->data.identifier.text_len == 1);
        EXPECT(tp->data.typeParameter.name->data.identifier.text[0] == 'T');
    }
    EXPECT(md->data.methodDeclaration.parameters.len == 0);
    EXPECT(md->data.methodDeclaration.body != NULL);
    EXPECT(md->data.methodDeclaration.body->kind == CTSC_SK_Block);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseObjectLiteralElement (~6709) +
     * parseAccessorDeclaration (~7851) for the
     * 108_parserComputedPropertyName4.ts shape
     *     //@target: ES6
     *     var v = { get [e]() { } };
     * The `get` contextual keyword is consumed by parseContextualModifier
     * (~2754) when the next token is `[` (canFollowGetOrSetKeyword ~2819 /
     * isLiteralPropertyName ~2703). parsePropertyName then produces a
     * ComputedPropertyName (~4212). The GetAccessor's children (for
     * forEachChildInGetAccessor ~617) are [name, body]. */
    ctsc_arena_init(&a, 4096);
    src = "//@target: ES6\nvar v = { get [e]() { } };";
    src_len = (int)strlen(src);
    r = ctsc_parse(src, (size_t)src_len, &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_VariableStatement);
    decl = s->data.variableStatement.declarationList
        ->data.variableDeclarationList.declarations.items[0];
    obj = decl->data.variableDeclaration.initializer;
    EXPECT(obj != NULL);
    EXPECT(obj->kind == CTSC_SK_ObjectLiteralExpression);
    EXPECT(obj->data.objectLiteralExpression.properties.len == 1);
    {
        const CtscNode* acc = obj->data.objectLiteralExpression.properties.items[0];
        EXPECT(acc->kind == CTSC_SK_GetAccessor);
        /* Positions for the `\n`-only C literal (no CRLF):
         *   //@target: ES6\nvar v = { get [e]() { } };
         *   0         1         2         3         4
         *   01234567890123456789012345678901234567890
         * `get` token starts at 25, full_start (leading space) at 24.
         * Body `{ }` closes at 37; scanner's next full_start is 38, so
         * Block.end = 38, and GetAccessor.end = 38 (finishNode end =
         * scanner.getTokenFullStart() after the body). */
        EXPECT(acc->pos == 24);
        EXPECT(acc->end == 38);
        const CtscNode* nm = acc->data.accessorDeclaration.name;
        EXPECT(nm != NULL);
        EXPECT(nm->kind == CTSC_SK_ComputedPropertyName);
        /* ComputedPropertyName.pos = full_start of `[` (space at 28 is
         * leading trivia), ComputedPropertyName.end = full_start of `(`. */
        EXPECT(nm->pos == 28);
        EXPECT(nm->end == 32);
        EXPECT(acc->data.accessorDeclaration.parameters.len == 0);
        const CtscNode* gbody = acc->data.accessorDeclaration.body;
        EXPECT(gbody != NULL);
        EXPECT(gbody->kind == CTSC_SK_Block);
        EXPECT(gbody->data.block.statements.len == 0);
    }
    ctsc_arena_free(&a);

    /* `get` and `set` as bare property names must NOT be consumed as
     * contextual modifiers when the next token is `:` or `}` (the
     * ShorthandPropertyAssignment / PropertyAssignment path). Mirrors
     * upstream's parseContextualModifier(~2754) tryParse failure branch:
     * canFollowGetOrSetKeyword(~2819) rejects `:` and `}`, so parseObjectLiteralElement
     * falls through to the standard identifier-PropertyName handling. */
    ctsc_arena_init(&a, 4096);
    src = "var v = { get: 1 };";
    r = ctsc_parse(src, strlen(src), &a);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    decl = s->data.variableStatement.declarationList
        ->data.variableDeclarationList.declarations.items[0];
    obj = decl->data.variableDeclaration.initializer;
    EXPECT(obj->kind == CTSC_SK_ObjectLiteralExpression);
    EXPECT(obj->data.objectLiteralExpression.properties.len == 1);
    {
        const CtscNode* pa = obj->data.objectLiteralExpression.properties.items[0];
        EXPECT(pa->kind == CTSC_SK_PropertyAssignment);
        EXPECT(pa->data.propertyAssignment.name->kind == CTSC_SK_Identifier);
        EXPECT(pa->data.propertyAssignment.name->data.identifier.text_len == 3);
        EXPECT(pa->data.propertyAssignment.name->data.identifier.text[0] == 'g');
    }
    ctsc_arena_free(&a);

    /* Empty object literal `({})` is still an ObjectLiteralExpression with no
     * properties — the JSON emitter should NOT emit a `children` key in that
     * case (oracle's default branch uses `if (children.length) …`). */
    ctsc_arena_init(&a, 4096);
    src = "({});";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* paren = s->data.expressionStatement.expression;
    EXPECT(paren->kind == CTSC_SK_ParenthesizedExpression);
    EXPECT(paren->data.parenthesizedExpression.expression->kind == CTSC_SK_ObjectLiteralExpression);
    EXPECT(paren->data.parenthesizedExpression.expression
        ->data.objectLiteralExpression.properties.len == 0);
    ctsc_arena_free(&a);

    /* Empty parens `()` in expression position: mirrors upstream parser.ts
     * parseParenthesizedExpression (~6663) + parsePrimaryExpression's final
     * fall-through parseIdentifier(Diagnostics.Expression_expected) (~6660).
     * For the 106_parser566700.ts fixture `var v = ()({});` the inner `()` is
     * a ParenthesizedExpression whose expression is a zero-width missing
     * Identifier at the `)` full_start (empty escapedText), NOT null.
     * Use `()();` here to avoid depending on the `// @target:` directive
     * alignment — positions still pin the missing-Identifier shape. */
    ctsc_arena_init(&a, 4096);
    src = "()();";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* call = s->data.expressionStatement.expression;
    EXPECT(call->kind == CTSC_SK_CallExpression);
    const CtscNode* callee = call->data.callExpression.expression;
    EXPECT(callee != NULL);
    EXPECT(callee->kind == CTSC_SK_ParenthesizedExpression);
    const CtscNode* inner = callee->data.parenthesizedExpression.expression;
    EXPECT(inner != NULL);
    EXPECT(inner->kind == CTSC_SK_Identifier);
    /* Missing identifier is zero-width at scanner.getTokenFullStart() which is
     * the `)` position = 1. */
    EXPECT(inner->pos == 1);
    EXPECT(inner->end == 1);
    EXPECT(inner->data.identifier.text_len == 0);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseClassDeclarationOrExpression (~8154)
     * for the 106_parser512084.ts fixture `// @target: es2015\r\nclass foo {\r\n`
     * (an unterminated class). The fixture expects a ClassDeclaration with
     * pos=0, end=31 (full_start of EOF, before the trailing \r\n trivia is
     * absorbed as EOF's leading trivia), containing a single child — the
     * Identifier `foo` with pos=25 (full_start, including the leading space
     * after `class`) and end=29 (full_start of the next token `{`). */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\r\nclass foo {\r\n";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ClassDeclaration);
    EXPECT(s->pos == 0);
    EXPECT(s->end == 31);
    const CtscNode* cname = s->data.classDeclaration.name;
    EXPECT(cname != NULL);
    EXPECT(cname->kind == CTSC_SK_Identifier);
    EXPECT(cname->pos == 25);
    EXPECT(cname->end == 29);
    EXPECT(cname->data.identifier.text_len == 3);
    EXPECT(s->data.classDeclaration.members.len == 0);
    ctsc_arena_free(&a);

    /* Well-formed empty class: the `}` is consumed so finishNode end = the
     * full_start of the next token, which includes the trailing semicolon. */
    ctsc_arena_init(&a, 4096);
    src = "class A {}";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ClassDeclaration);
    EXPECT(s->pos == 0);
    EXPECT(s->end == 10);
    EXPECT(s->data.classDeclaration.name != NULL);
    EXPECT(s->data.classDeclaration.name->kind == CTSC_SK_Identifier);
    EXPECT(s->data.classDeclaration.members.len == 0);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseClassElement (~8068) →
     * parsePropertyOrMethodDeclaration (~7835) → parseMethodDeclaration
     * (~7782) for the 107_MemberFunctionDeclaration5_es6.ts fixture:
     *   // @target: es6\r\nclass C {\r\n   *\r\n}
     * The `*` starts a generator MethodDeclaration whose PropertyName falls
     * back to createMissingNode(Identifier) — parsePropertyName / parseIdentifierName
     * synthesise a zero-width Identifier at scanner.getTokenFullStart(), which
     * is the full_start of the following `}` (= 32). parseParameters fails
     * (no `(`), parseFunctionBlockOrSemicolon returns undefined, and
     * finishNode(md, pos=26) records end = cur_full_start = 32.
     * AsteriskToken spans from full_start of `*` (= 26, includes the \r\n + 3
     * leading spaces as leading trivia) to scanner.getTokenFullStart() AFTER
     * consuming `*` (= 32, the full_start of `}`). */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es6\r\nclass C {\r\n   *\r\n}";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ClassDeclaration);
    EXPECT(s->pos == 0);
    EXPECT(s->end == 35);
    EXPECT(s->data.classDeclaration.members.len == 1);
    const CtscNode* cem = s->data.classDeclaration.members.items[0];
    EXPECT(cem->kind == CTSC_SK_MethodDeclaration);
    EXPECT(cem->pos == 26);
    EXPECT(cem->end == 32);
    EXPECT(cem->data.methodDeclaration.has_asterisk);
    EXPECT(cem->data.methodDeclaration.asterisk_pos == 26);
    EXPECT(cem->data.methodDeclaration.asterisk_end == 32);
    EXPECT(cem->data.methodDeclaration.name != NULL);
    EXPECT(cem->data.methodDeclaration.name->kind == CTSC_SK_Identifier);
    EXPECT(cem->data.methodDeclaration.name->pos == 32);
    EXPECT(cem->data.methodDeclaration.name->end == 32);
    EXPECT(cem->data.methodDeclaration.name->data.identifier.text_len == 0);
    EXPECT(cem->data.methodDeclaration.parameters.len == 0);
    EXPECT(cem->data.methodDeclaration.type_parameters.len == 0);
    EXPECT(cem->data.methodDeclaration.body == NULL);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parsePrimaryExpression (~6642, case
     * ClassKeyword) → parseClassExpression → parseClassDeclarationOrExpression
     * with kind=ClassExpression. Fixture 107_classExpression1.ts:
     *   // @target: es2015\nvar v = class C {};
     * expects a single VariableStatement whose VariableDeclaration carries
     * a ClassExpression initializer (pos=26, end=37) named `C`. */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\nvar v = class C {};";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_VariableStatement);
    EXPECT(s->pos == 0);
    EXPECT(s->end == 38);
    const CtscNode* ceList = s->data.variableStatement.declarationList;
    EXPECT(ceList != NULL);
    EXPECT(ceList->kind == CTSC_SK_VariableDeclarationList);
    EXPECT(ceList->data.variableDeclarationList.declarations.len == 1);
    const CtscNode* ceDecl = ceList->data.variableDeclarationList.declarations.items[0];
    EXPECT(ceDecl->kind == CTSC_SK_VariableDeclaration);
    EXPECT(ceDecl->pos == 22);
    EXPECT(ceDecl->end == 37);
    const CtscNode* ceInit = ceDecl->data.variableDeclaration.initializer;
    EXPECT(ceInit != NULL);
    EXPECT(ceInit->kind == CTSC_SK_ClassExpression);
    EXPECT(ceInit->pos == 26);
    EXPECT(ceInit->end == 37);
    EXPECT(ceInit->data.classDeclaration.name != NULL);
    EXPECT(ceInit->data.classDeclaration.name->kind == CTSC_SK_Identifier);
    EXPECT(ceInit->data.classDeclaration.name->pos == 32);
    EXPECT(ceInit->data.classDeclaration.name->end == 34);
    EXPECT(ceInit->data.classDeclaration.members.len == 0);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseDelimitedList (~3489) error recovery
     * for ParsingContext.VariableDeclarations via
     * abortParsingListOrMoveToNextToken (~3410): an unexpected token that
     * cannot start another declaration AND is not a list terminator AND is
     * NOT claimed by any outer parsing context (SourceElements.isListElement
     * == isStartOfStatement) is consumed via nextToken(). The 106_parser645086_1.ts
     * fixture `var v = /[]/]/` trips this: after parsing the regex initializer
     * the scanner sits at `]` (pos 31) — which is neither a statement start
     * nor a list terminator — so tsc advances to the next `/` (pos 32) which
     * IS a statement start (SlashToken). Consequently VariableDeclarationList
     * and VariableStatement both finish at pos 32, not 31. */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\nvar v = /[]/]/";
    int vs_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)vs_end, &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 2);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_VariableStatement);
    EXPECT(s->pos == 0);
    EXPECT(s->end == 32);
    const CtscNode* vdl = s->data.variableStatement.declarationList;
    EXPECT(vdl->kind == CTSC_SK_VariableDeclarationList);
    EXPECT(vdl->pos == 0);
    EXPECT(vdl->end == 32);
    EXPECT(vdl->data.variableDeclarationList.declarations.len == 1);
    const CtscNode* vd = vdl->data.variableDeclarationList.declarations.items[0];
    EXPECT(vd->pos == 22);
    EXPECT(vd->end == 31);
    EXPECT(vd->data.variableDeclaration.initializer != NULL);
    EXPECT(vd->data.variableDeclaration.initializer->kind == CTSC_SK_RegularExpressionLiteral);
    EXPECT(vd->data.variableDeclaration.initializer->pos == 26);
    EXPECT(vd->data.variableDeclaration.initializer->end == 31);
    const CtscNode* s2 = r.sourceFile->data.sourceFile.statements.items[1];
    EXPECT(s2->kind == CTSC_SK_ExpressionStatement);
    EXPECT(s2->pos == 32);
    EXPECT(s2->end == 33);
    EXPECT(s2->data.expressionStatement.expression->kind == CTSC_SK_RegularExpressionLiteral);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseCallExpressionRest (~6520) +
     * parseTypeArgumentsInExpression (~6562): `f(g<A, B>(7))` must parse
     * `g<A, B>(7)` as a CallExpression with `g` as the callee and `7` as
     * the sole argument (the type arguments `<A, B>` are absorbed but not
     * serialized — the oracle does not visit CallExpression.typeArguments).
     * Without the speculative type-arguments parse, `<` would be consumed
     * as a relational operator and the outer call would end up with two
     * BinaryExpression arguments instead of a single nested CallExpression.
     * Exercises the 106_parserAmbiguity1.ts fixture shape. */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\nf(g<A, B>(7));";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* outer_call = s->data.expressionStatement.expression;
    EXPECT(outer_call->kind == CTSC_SK_CallExpression);
    EXPECT(outer_call->data.callExpression.arguments.len == 1);
    const CtscNode* inner_call = outer_call->data.callExpression.arguments.items[0];
    EXPECT(inner_call->kind == CTSC_SK_CallExpression);
    EXPECT(inner_call->pos == 21);
    EXPECT(inner_call->end == 31);
    EXPECT(inner_call->data.callExpression.expression->kind == CTSC_SK_Identifier);
    EXPECT(inner_call->data.callExpression.arguments.len == 1);
    EXPECT(inner_call->data.callExpression.arguments.items[0]->kind == CTSC_SK_NumericLiteral);
    ctsc_arena_free(&a);

    /* Counter-case: `f(g<A, B>7)` — after `<A, B>` the follow token is a
     * NumericLiteral (`7`), which fails canFollowTypeArgumentsInExpression
     * (parser.ts ~6587). Speculative parse rolls back, `<` is consumed as
     * a relational operator, and the outer call ends up with TWO arguments:
     * `g<A` and `B>7`. Mirrors the 106_parserAmbiguity2.ts fixture. */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\nf(g<A, B>7);";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* ambiguous_call = s->data.expressionStatement.expression;
    EXPECT(ambiguous_call->kind == CTSC_SK_CallExpression);
    EXPECT(ambiguous_call->data.callExpression.arguments.len == 2);
    EXPECT(ambiguous_call->data.callExpression.arguments.items[0]->kind == CTSC_SK_BinaryExpression);
    EXPECT(ambiguous_call->data.callExpression.arguments.items[1]->kind == CTSC_SK_BinaryExpression);
    ctsc_arena_free(&a);

    /* Regression guard: a plain relational expression `a < b` at statement
     * position must remain a BinaryExpression even though the call-rest
     * logic speculatively probes `<` as a type-argument starter. The
     * rollback path (no `(` follow, no other disambiguating token) is
     * exercised here. */
    ctsc_arena_init(&a, 4096);
    src = "a < b;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    EXPECT(s->data.expressionStatement.expression->kind == CTSC_SK_BinaryExpression);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseObjectLiteralElement (~6699) +
     * parsePropertyAssignment (~6743) for the 106_parserComputedPropertyName1.ts
     * fixture `//@target: ES6\r\nvar v = { [e] };`. The inner `{ [e] }` is a
     * PropertyAssignment whose name is a ComputedPropertyName `[e]` and whose
     * initializer is a zero-width missing Identifier (parseExpected(ColonToken)
     * fails on `}`; parseAssignmentExpressionOrHigher falls through to
     * parseIdentifier(Expression_expected) synthesising the missing node at
     * scanner.getTokenFullStart()). */
    ctsc_arena_init(&a, 4096);
    src = "//@target: ES6\r\nvar v = { [e] };";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_VariableStatement);
    const CtscNode* vd2 = s->data.variableStatement.declarationList
        ->data.variableDeclarationList.declarations.items[0];
    EXPECT(vd2->kind == CTSC_SK_VariableDeclaration);
    const CtscNode* obj2 = vd2->data.variableDeclaration.initializer;
    EXPECT(obj2 != NULL);
    EXPECT(obj2->kind == CTSC_SK_ObjectLiteralExpression);
    /* `{` full_start = 23 (leading space after `=`); `}` end = 31 (before `;`). */
    EXPECT(obj2->pos == 23);
    EXPECT(obj2->end == 31);
    EXPECT(obj2->data.objectLiteralExpression.properties.len == 1);
    const CtscNode* pa2 = obj2->data.objectLiteralExpression.properties.items[0];
    EXPECT(pa2->kind == CTSC_SK_PropertyAssignment);
    /* PropertyAssignment.pos = full_start of `[` = 25 (leading space after `{`).
     * PropertyAssignment.end = cur_full_start after missing initializer = 29. */
    EXPECT(pa2->pos == 25);
    EXPECT(pa2->end == 29);
    const CtscNode* cpn = pa2->data.propertyAssignment.name;
    EXPECT(cpn != NULL);
    EXPECT(cpn->kind == CTSC_SK_ComputedPropertyName);
    EXPECT(cpn->pos == 25);
    EXPECT(cpn->end == 29);
    const CtscNode* cpe = cpn->data.computedPropertyName.expression;
    EXPECT(cpe != NULL);
    EXPECT(cpe->kind == CTSC_SK_Identifier);
    /* `e` full_start = 27 (no trivia before it inside `[e`); end = 28. */
    EXPECT(cpe->pos == 27);
    EXPECT(cpe->end == 28);
    const CtscNode* pa2_init = pa2->data.propertyAssignment.initializer;
    EXPECT(pa2_init != NULL);
    EXPECT(pa2_init->kind == CTSC_SK_Identifier);
    /* Zero-width missing Identifier at the `}` full_start = 29. */
    EXPECT(pa2_init->pos == 29);
    EXPECT(pa2_init->end == 29);
    EXPECT(pa2_init->data.identifier.text_len == 0);
    ctsc_arena_free(&a);

    /* ShorthandPropertyAssignment guard: `({ a })` — identifier name, no
     * colon, no `=`. Both Name and children list should be single-element. */
    ctsc_arena_init(&a, 4096);
    src = "({ a });";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* paren2 = s->data.expressionStatement.expression;
    EXPECT(paren2->kind == CTSC_SK_ParenthesizedExpression);
    const CtscNode* obj3 = paren2->data.parenthesizedExpression.expression;
    EXPECT(obj3->kind == CTSC_SK_ObjectLiteralExpression);
    EXPECT(obj3->data.objectLiteralExpression.properties.len == 1);
    const CtscNode* sp = obj3->data.objectLiteralExpression.properties.items[0];
    EXPECT(sp->kind == CTSC_SK_ShorthandPropertyAssignment);
    EXPECT(sp->data.shorthandPropertyAssignment.name != NULL);
    EXPECT(sp->data.shorthandPropertyAssignment.name->kind == CTSC_SK_Identifier);
    EXPECT(sp->data.shorthandPropertyAssignment.objectAssignmentInitializer == NULL);
    ctsc_arena_free(&a);

    /* PropertyAssignment with identifier key and a real initializer:
     * `({ a: 1 });` should produce a PropertyAssignment whose name is an
     * Identifier `a` and whose initializer is the NumericLiteral `1`. */
    ctsc_arena_init(&a, 4096);
    src = "({ a: 1 });";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* paren3 = s->data.expressionStatement.expression;
    const CtscNode* obj4 = paren3->data.parenthesizedExpression.expression;
    EXPECT(obj4->kind == CTSC_SK_ObjectLiteralExpression);
    EXPECT(obj4->data.objectLiteralExpression.properties.len == 1);
    const CtscNode* pa3 = obj4->data.objectLiteralExpression.properties.items[0];
    EXPECT(pa3->kind == CTSC_SK_PropertyAssignment);
    EXPECT(pa3->data.propertyAssignment.name->kind == CTSC_SK_Identifier);
    EXPECT(pa3->data.propertyAssignment.initializer->kind == CTSC_SK_NumericLiteral);
    ctsc_arena_free(&a);

    /* ObjectLiteral error recovery: mirrors upstream
     * parser.ts parseDelimitedList (~3492) considerSemicolonAsDelimiter=true
     * path for parseObjectLiteralExpression (~6760). Fixture
     * 108_parserErrorRecovery_ObjectLiteral2.ts is `var v = { a\nreturn;`:
     *   - `a` on line 2 parses as a ShorthandPropertyAssignment (no colon,
     *     tokenIsIdentifier true).
     *   - On line 3, `return` is a keyword and therefore isLiteralPropertyName
     *     (parser.ts ~2703), so isListElement(ObjectLiteralMembers, …) is true
     *     and we enter parseObjectLiteralElement again. The element takes the
     *     PropertyAssignment path because `return` is NOT isIdentifier
     *     (reserved word); parseExpected(ColonToken) fails, leaving a
     *     missing-identifier initializer at scanner.getTokenFullStart().
     *   - Back in the delimited list, parseOptional(CommaToken) is false;
     *     isListTerminator(ObjectLiteralMembers) is false at `;`;
     *     parseExpected(CommaToken) pushes a diagnostic; then
     *     considerSemicolonAsDelimiter=true consumes the `;` (same-line, no
     *     preceding line break), advancing to EOF; next iteration
     *     isListTerminator is true.
     * The BUG was that ctsc's old loop synthesised a third, empty
     * PropertyAssignment (kind=PropertyAssignment, pos==end==39, two missing
     * Identifier children) at the `;` instead of consuming it as a delimiter,
     * failing the fixture's byte-exact AST diff. */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\r\nvar v = { a\r\nreturn;";
    int obj_rec_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)obj_rec_end, &a);
    EXPECT(obj_rec_end == 40);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_VariableStatement);
    const CtscNode* decl_rec = s->data.variableStatement.declarationList
        ->data.variableDeclarationList.declarations.items[0];
    const CtscNode* obj_rec = decl_rec->data.variableDeclaration.initializer;
    EXPECT(obj_rec != NULL);
    EXPECT(obj_rec->kind == CTSC_SK_ObjectLiteralExpression);
    EXPECT(obj_rec->pos == 27);
    EXPECT(obj_rec->end == 40);
    EXPECT(obj_rec->data.objectLiteralExpression.properties.len == 2);
    const CtscNode* sp_rec = obj_rec->data.objectLiteralExpression.properties.items[0];
    EXPECT(sp_rec->kind == CTSC_SK_ShorthandPropertyAssignment);
    EXPECT(sp_rec->pos == 29);
    EXPECT(sp_rec->end == 31);
    EXPECT(sp_rec->data.shorthandPropertyAssignment.name->kind == CTSC_SK_Identifier);
    EXPECT(sp_rec->data.shorthandPropertyAssignment.name->data.identifier.text_len == 1);
    EXPECT(sp_rec->data.shorthandPropertyAssignment.name->data.identifier.text[0] == 'a');
    const CtscNode* pa_rec = obj_rec->data.objectLiteralExpression.properties.items[1];
    EXPECT(pa_rec->kind == CTSC_SK_PropertyAssignment);
    EXPECT(pa_rec->pos == 31);
    EXPECT(pa_rec->end == 39);
    EXPECT(pa_rec->data.propertyAssignment.name->kind == CTSC_SK_Identifier);
    EXPECT(pa_rec->data.propertyAssignment.name->data.identifier.text_len == 6);
    EXPECT(pa_rec->data.propertyAssignment.name->data.identifier.text[0] == 'r');
    EXPECT(pa_rec->data.propertyAssignment.initializer->kind == CTSC_SK_Identifier);
    EXPECT(pa_rec->data.propertyAssignment.initializer->pos == 39);
    EXPECT(pa_rec->data.propertyAssignment.initializer->end == 39);
    EXPECT(pa_rec->data.propertyAssignment.initializer->data.identifier.text_len == 0);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseEnumDeclaration (~8275): `enum void {}`
     * (106_parserEnumDeclaration4.ts). `void` is a reserved word so
     * parseIdentifier's createIdentifier branch falls through to
     * createMissingNode(Identifier) (~2619) — a zero-width Identifier at
     * scanner.getTokenFullStart() with empty escapedText. parseExpected(OpenBrace)
     * then fails because the current token is still `void`, so members is
     * the missing list and finishNode.end = full_start of `void`. The outer
     * statement loop then parses `void { }` as VoidExpression(ObjectLiteral)
     * wrapped in an ExpressionStatement. */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\r\nenum void {\r\n}";
    int enum_src_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)enum_src_end, &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 2);
    const CtscNode* ed = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(ed->kind == CTSC_SK_EnumDeclaration);
    /* enum keyword full_start = 0 (comment + CRLF is its leading trivia);
     * end = full_start of `void` = 24 (the space between `m` and `v` is
     * leading trivia of `void`). */
    EXPECT(ed->pos == 0);
    EXPECT(ed->end == 24);
    const CtscNode* ed_name = ed->data.enumDeclaration.name;
    EXPECT(ed_name != NULL);
    EXPECT(ed_name->kind == CTSC_SK_Identifier);
    EXPECT(ed_name->pos == 24 && ed_name->end == 24);
    EXPECT(ed_name->data.identifier.text_len == 0);
    EXPECT(ed->data.enumDeclaration.members.len == 0);
    const CtscNode* es = r.sourceFile->data.sourceFile.statements.items[1];
    EXPECT(es->kind == CTSC_SK_ExpressionStatement);
    EXPECT(es->pos == 24 && es->end == enum_src_end);
    EXPECT(es->data.expressionStatement.expression->kind == CTSC_SK_VoidExpression);
    ctsc_arena_free(&a);

    /* Well-formed empty enum: `enum E {}`. name is consumed Identifier;
     * members is empty; EnumDeclaration.end = full_start of token AFTER `}`. */
    ctsc_arena_init(&a, 4096);
    src = "enum E {}";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_EnumDeclaration);
    EXPECT(s->data.enumDeclaration.name != NULL);
    EXPECT(s->data.enumDeclaration.name->kind == CTSC_SK_Identifier);
    EXPECT(s->data.enumDeclaration.name->data.identifier.text_len == 1);
    EXPECT(s->data.enumDeclaration.members.len == 0);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseClassElement (~8068) →
     * parsePropertyOrMethodDeclaration (~7835) → parsePropertyDeclaration
     * (~7814) for the 108_parserComputedPropertyName10.ts fixture:
     *   //@target: ES6\r\nclass C {\r\n   [e] = 1\r\n}
     * The class body contains a single PropertyDeclaration whose name is a
     * ComputedPropertyName `[e]` and whose initializer is the NumericLiteral
     * `1`. Positions (UTF-16 offsets, CRLF newlines):
     *   ClassDeclaration      pos=0,  end=40 (full_start of EOF)
     *     Identifier `C`      pos=21, end=23 (leading space after `class`)
     *     PropertyDeclaration pos=25, end=37 (full_start of name = \r\n after `{`)
     *       ComputedPropertyName pos=25, end=33 (end = full_start of `=`)
     *         Identifier `e` pos=31, end=32
     *       NumericLiteral `1` pos=35, end=37 (end = full_start of `}`)
     */
    ctsc_arena_init(&a, 4096);
    src = "//@target: ES6\r\nclass C {\r\n   [e] = 1\r\n}";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(strlen(src) == 40);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ClassDeclaration);
    EXPECT(s->pos == 0 && s->end == 40);
    EXPECT(s->data.classDeclaration.members.len == 1);
    const CtscNode* pd = s->data.classDeclaration.members.items[0];
    EXPECT(pd->kind == CTSC_SK_PropertyDeclaration);
    EXPECT(pd->pos == 25 && pd->end == 37);
    const CtscNode* pd_name = pd->data.propertyDeclaration.name;
    EXPECT(pd_name != NULL);
    EXPECT(pd_name->kind == CTSC_SK_ComputedPropertyName);
    EXPECT(pd_name->pos == 25 && pd_name->end == 33);
    const CtscNode* pd_name_expr = pd_name->data.computedPropertyName.expression;
    EXPECT(pd_name_expr != NULL);
    EXPECT(pd_name_expr->kind == CTSC_SK_Identifier);
    EXPECT(pd_name_expr->pos == 31 && pd_name_expr->end == 32);
    const CtscNode* pd_init = pd->data.propertyDeclaration.initializer;
    EXPECT(pd_init != NULL);
    EXPECT(pd_init->kind == CTSC_SK_NumericLiteral);
    EXPECT(pd_init->pos == 35 && pd_init->end == 37);
    ctsc_arena_free(&a);

    /* Plain-identifier PropertyDeclaration without initializer or
     * type annotation: `class C { x; }` parses as a single
     * PropertyDeclaration whose name is Identifier `x` and whose
     * initializer is undefined. The `;` is consumed by
     * parseSemicolonAfterPropertyName. */
    ctsc_arena_init(&a, 4096);
    src = "class C { x; }";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ClassDeclaration);
    EXPECT(s->data.classDeclaration.members.len == 1);
    const CtscNode* pd2 = s->data.classDeclaration.members.items[0];
    EXPECT(pd2->kind == CTSC_SK_PropertyDeclaration);
    EXPECT(pd2->data.propertyDeclaration.name != NULL);
    EXPECT(pd2->data.propertyDeclaration.name->kind == CTSC_SK_Identifier);
    EXPECT(pd2->data.propertyDeclaration.type == NULL);
    EXPECT(pd2->data.propertyDeclaration.initializer == NULL);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parsePropertyDeclaration (~7814) for the
     * 108_parserComputedPropertyName9.ts fixture:
     *   //@target: ES6\r\nclass C {\r\n   [e]: Type\r\n}
     * The class body contains a single PropertyDeclaration with a
     * ComputedPropertyName `[e]` and a TypeReference `: Type` — no
     * initializer. The oracle surfaces `type` as a PropertyDeclaration
     * child (forEachChildInPropertyDeclaration ~536) and as a
     * TypeReference carrying typeName=Identifier(Type). Positions
     * (UTF-16 offsets, CRLF newlines):
     *   ClassDeclaration      pos=0,  end=42
     *     Identifier `C`      pos=21, end=23
     *     PropertyDeclaration pos=25, end=39
     *       ComputedPropertyName pos=25, end=33
     *         Identifier `e` pos=31, end=32
     *       TypeReference    pos=34, end=39
     *         Identifier `Type` pos=34, end=39
     */
    ctsc_arena_init(&a, 4096);
    src = "//@target: ES6\r\nclass C {\r\n   [e]: Type\r\n}";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(strlen(src) == 42);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ClassDeclaration);
    EXPECT(s->pos == 0 && s->end == 42);
    EXPECT(s->data.classDeclaration.members.len == 1);
    const CtscNode* pd3 = s->data.classDeclaration.members.items[0];
    EXPECT(pd3->kind == CTSC_SK_PropertyDeclaration);
    EXPECT(pd3->pos == 25 && pd3->end == 39);
    EXPECT(pd3->data.propertyDeclaration.name != NULL);
    EXPECT(pd3->data.propertyDeclaration.name->kind == CTSC_SK_ComputedPropertyName);
    const CtscNode* pd3_type = pd3->data.propertyDeclaration.type;
    EXPECT(pd3_type != NULL);
    EXPECT(pd3_type->kind == CTSC_SK_TypeReference);
    EXPECT(pd3_type->pos == 34 && pd3_type->end == 39);
    const CtscNode* pd3_type_name = pd3_type->data.typeReference.typeName;
    EXPECT(pd3_type_name != NULL);
    EXPECT(pd3_type_name->kind == CTSC_SK_Identifier);
    EXPECT(pd3_type_name->pos == 34 && pd3_type_name->end == 39);
    EXPECT(pd3_type_name->data.identifier.text_len == 4);
    EXPECT(pd3->data.propertyDeclaration.initializer == NULL);
    ctsc_arena_free(&a);

    /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseStatement's
     * FinallyKeyword dispatch (~7431): a bare `finally` at statement start
     * still parses as a TryStatement via parseTryStatement (~7078). With a
     * missing `try` keyword parseBlock's openBraceParsed=false branch
     * (~6841) yields a zero-width tryBlock at the current full_start; the
     * `finally` token is then consumed via parseExpected(FinallyKeyword,
     * catch_or_finally_expected) and the following parseBlock produces a
     * second zero-width Block at EOF's full_start. Reproduces the
     * 106_parserMissingToken1.ts fixture (`// @target: es2015\na / finally`):
     * the `/` binary expression is one statement (missing right identifier
     * at full_start of `finally` = 22), then TryStatement(pos=22, end=30)
     * with two empty Blocks at [22,22] and [30,30]. */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\na / finally";
    int try_src_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)try_src_end, &a);
    EXPECT(try_src_end == 30);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 2);
    const CtscNode* es0 = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(es0->kind == CTSC_SK_ExpressionStatement);
    EXPECT(es0->pos == 0 && es0->end == 22);
    EXPECT(es0->data.expressionStatement.expression->kind == CTSC_SK_BinaryExpression);
    const CtscNode* ts_stmt = r.sourceFile->data.sourceFile.statements.items[1];
    EXPECT(ts_stmt->kind == CTSC_SK_TryStatement);
    EXPECT(ts_stmt->pos == 22 && ts_stmt->end == 30);
    EXPECT(ts_stmt->data.tryStatement.tryBlock != NULL);
    EXPECT(ts_stmt->data.tryStatement.tryBlock->kind == CTSC_SK_Block);
    EXPECT(ts_stmt->data.tryStatement.tryBlock->pos == 22);
    EXPECT(ts_stmt->data.tryStatement.tryBlock->end == 22);
    EXPECT(ts_stmt->data.tryStatement.tryBlock->data.block.statements.len == 0);
    EXPECT(ts_stmt->data.tryStatement.catchClause == NULL);
    EXPECT(ts_stmt->data.tryStatement.finallyBlock != NULL);
    EXPECT(ts_stmt->data.tryStatement.finallyBlock->kind == CTSC_SK_Block);
    EXPECT(ts_stmt->data.tryStatement.finallyBlock->pos == 30);
    EXPECT(ts_stmt->data.tryStatement.finallyBlock->end == 30);
    EXPECT(ts_stmt->data.tryStatement.finallyBlock->data.block.statements.len == 0);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseInterfaceDeclaration (~8239) for the
     * 108_parserErrorRecovery_ExtendsOrImplementsClause6.ts fixture
     * (`// @target: es2015\ninterface I extends { }`). The trailing `{ }`
     * fails isValidHeritageClauseObjectLiteral (peek past `{}` lands on
     * EOF, which is not in Comma/OpenBrace/Extends/Implements), so
     * parseDelimitedList(HeritageClauseElement) keeps `types` empty and
     * the scanner stays on `{`. HeritageClause finishNode's end = full_start
     * of `{` (= 38). parseObjectTypeMembers then consumes `{ }` as the
     * interface body, and InterfaceDeclaration.end = full_start of EOF (= 42).
     * Expected children: [Identifier(I)@[28,30), HeritageClause@[30,38)]. */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\ninterface I extends { }";
    int iface_src_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)iface_src_end, &a);
    EXPECT(iface_src_end == 42);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    const CtscNode* idecl = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(idecl->kind == CTSC_SK_InterfaceDeclaration);
    EXPECT(idecl->pos == 0 && idecl->end == 42);
    EXPECT(idecl->data.classDeclaration.name != NULL);
    EXPECT(idecl->data.classDeclaration.name->kind == CTSC_SK_Identifier);
    EXPECT(idecl->data.classDeclaration.name->pos == 28);
    EXPECT(idecl->data.classDeclaration.name->end == 30);
    EXPECT(idecl->data.classDeclaration.heritage_clauses.len == 1);
    const CtscNode* hc = idecl->data.classDeclaration.heritage_clauses.items[0];
    EXPECT(hc->kind == CTSC_SK_HeritageClause);
    EXPECT(hc->pos == 30 && hc->end == 38);
    EXPECT(hc->data.heritageClause.token == CTSC_SK_ExtendsKeyword);
    EXPECT(hc->data.heritageClause.types.len == 0);
    EXPECT(idecl->data.classDeclaration.members.len == 0);
    ctsc_arena_free(&a);

    /* `export interface` — parser.ts parseDeclaration (~7467) + ExportKeyword
     * arm routing to parseInterfaceDeclaration. Selfhost-derived
     * 97_empty_module_marker.ts. */
    ctsc_arena_init(&a, 4096);
    src = "export interface I {}";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    idecl = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(idecl->kind == CTSC_SK_InterfaceDeclaration);
    EXPECT(idecl->data.classDeclaration.modifiers.len == 1);
    EXPECT(idecl->data.classDeclaration.modifiers.items[0]->kind == CTSC_SK_ExportKeyword);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseObjectBindingPattern (~7612) +
     * parseObjectBindingElement (~7594) for the
     * 108_parserForOfStatement13.ts fixture (`for (let {a, b} of X) { }`).
     * The ObjectBindingPattern should hold two shorthand BindingElements,
     * each wrapping the identifier as its `name` (with a null propertyName),
     * so the AST-JSON default-branch serialiser emits
     * `children: [{kind: Identifier, ...}]` for each element. */
    ctsc_arena_init(&a, 4096);
    src = "for (let {a, b} of X) {\n}";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    const CtscNode* forOf = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(forOf->kind == CTSC_SK_ForOfStatement);
    const CtscNode* for_vdl = forOf->data.forInOrOfStatement.initializer;
    EXPECT(for_vdl != NULL);
    EXPECT(for_vdl->kind == CTSC_SK_VariableDeclarationList);
    EXPECT(for_vdl->data.variableDeclarationList.declarations.len == 1);
    const CtscNode* for_vd = for_vdl->data.variableDeclarationList.declarations.items[0];
    EXPECT(for_vd->kind == CTSC_SK_VariableDeclaration);
    const CtscNode* pat = for_vd->data.variableDeclaration.name;
    EXPECT(pat != NULL);
    EXPECT(pat->kind == CTSC_SK_ObjectBindingPattern);
    EXPECT(pat->data.bindingPattern.elements.len == 2);
    const CtscNode* be0 = pat->data.bindingPattern.elements.items[0];
    EXPECT(be0->kind == CTSC_SK_BindingElement);
    EXPECT(be0->data.bindingElement.propertyName == NULL);
    EXPECT(be0->data.bindingElement.initializer == NULL);
    EXPECT(be0->data.bindingElement.has_dotdotdot == false);
    EXPECT(be0->data.bindingElement.name != NULL);
    EXPECT(be0->data.bindingElement.name->kind == CTSC_SK_Identifier);
    EXPECT(be0->data.bindingElement.name->data.identifier.text_len == 1);
    EXPECT(be0->data.bindingElement.name->data.identifier.text[0] == 'a');
    const CtscNode* be1 = pat->data.bindingPattern.elements.items[1];
    EXPECT(be1->kind == CTSC_SK_BindingElement);
    EXPECT(be1->data.bindingElement.propertyName == NULL);
    EXPECT(be1->data.bindingElement.name != NULL);
    EXPECT(be1->data.bindingElement.name->kind == CTSC_SK_Identifier);
    EXPECT(be1->data.bindingElement.name->data.identifier.text[0] == 'b');
    ctsc_arena_free(&a);

    /* Array binding pattern counterpart: `for (let [a, b] of X) { }` (mirrors
     * 108_parserForOfStatement14.ts). parseArrayBindingElement (~7583) builds
     * a BindingElement whose name is the bare Identifier (no propertyName,
     * no dotDotDotToken), so the default-branch serialiser emits the same
     * single-child shape as the object form. */
    ctsc_arena_init(&a, 4096);
    src = "for (let [a, b] of X) {\n}";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    forOf = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(forOf->kind == CTSC_SK_ForOfStatement);
    for_vd = forOf->data.forInOrOfStatement.initializer
        ->data.variableDeclarationList.declarations.items[0];
    pat = for_vd->data.variableDeclaration.name;
    EXPECT(pat->kind == CTSC_SK_ArrayBindingPattern);
    EXPECT(pat->data.bindingPattern.elements.len == 2);
    be0 = pat->data.bindingPattern.elements.items[0];
    EXPECT(be0->kind == CTSC_SK_BindingElement);
    EXPECT(be0->data.bindingElement.name->kind == CTSC_SK_Identifier);
    EXPECT(be0->data.bindingElement.name->data.identifier.text[0] == 'a');
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseModuleDeclaration (~8338) +
     * parseModuleOrNamespaceDeclaration (~8303) + parseModuleBlock (~8290)
     * for the 108_parserModuleDeclaration6.ts fixture
     * (`// @target: es2015\r\nnamespace number {\r\n}`). isStartOfDeclaration
     * routes `namespace` + NumberKeyword-on-same-line into
     * parseModuleDeclaration; `number` (NumberKeyword) is accepted as an
     * identifier via parseIdentifier (~isIdentifier: token > LastReservedWord).
     * Expected shape (all UTF-16 / byte offsets):
     *   SourceFile [0, 41]
     *     ModuleDeclaration [0, 41]
     *       name: Identifier "number" [29, 36]
     *       body: ModuleBlock [36, 41] (empty) */
    ctsc_arena_init(&a, 4096);
    src = "// @target: es2015\r\nnamespace number {\r\n}";
    int mod_src_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)mod_src_end, &a);
    EXPECT(mod_src_end == 41);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    const CtscNode* mdecl = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(mdecl->kind == CTSC_SK_ModuleDeclaration);
    EXPECT(mdecl->pos == 0 && mdecl->end == 41);
    EXPECT(mdecl->data.moduleDeclaration.name != NULL);
    EXPECT(mdecl->data.moduleDeclaration.name->kind == CTSC_SK_Identifier);
    EXPECT(mdecl->data.moduleDeclaration.name->pos == 29);
    EXPECT(mdecl->data.moduleDeclaration.name->end == 36);
    EXPECT(mdecl->data.moduleDeclaration.name->data.identifier.text_len == 6);
    EXPECT(mdecl->data.moduleDeclaration.name->data.identifier.text[0] == 'n');
    EXPECT(mdecl->data.moduleDeclaration.body != NULL);
    EXPECT(mdecl->data.moduleDeclaration.body->kind == CTSC_SK_ModuleBlock);
    EXPECT(mdecl->data.moduleDeclaration.body->pos == 36);
    EXPECT(mdecl->data.moduleDeclaration.body->end == 41);
    EXPECT(mdecl->data.moduleDeclaration.body->data.moduleBlock.statements.len == 0);
    ctsc_arena_free(&a);

    /* `namespace` used as a bare identifier in expression position must NOT
     * route into parseModuleDeclaration. Mirrors isStartOfDeclaration's
     * nextTokenIsIdentifierOrStringLiteralOnSameLine check (~7208): when the
     * token after `namespace` is a `;` / EOF / non-identifier, the keyword
     * falls back to an Identifier-in-ExpressionStatement (parser.ts
     * parseStatement default branch via parseExpressionStatement). */
    ctsc_arena_init(&a, 4096);
    src = "namespace;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    const CtscNode* ns_stmt = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(ns_stmt->kind == CTSC_SK_ExpressionStatement);
    EXPECT(ns_stmt->data.expressionStatement.expression->kind == CTSC_SK_Identifier);
    ctsc_arena_free(&a);

    /* Mirrors upstream parser.ts parseTemplateExpression (~3668) +
     * parseLiteralOfTemplateSpan (~3713): once the substitution expression
     * has been parsed, the parser sees a CloseBraceToken and calls
     * reScanTemplateToken to resume the template literal, yielding a
     * TemplateMiddle (if another `${` follows) or a TemplateTail (if the
     * next backtick terminates the literal).
     *
     * Fixture 108_templateStringInDeleteExpression.ts (`delete `abc${0}abc`;`):
     *   tokens (UTF-16 start.end): delete [0..6) ` [7..8) abc [8..11)
     *   ${ [11..13) 0 [13..14) } [14..15) abc [15..18) ` [18..19) ; [19..20)
     *   AST nodes use full_start (= end of previous token) for `pos`:
     *     ExpressionStatement [0,20); DeleteExpression [0,19);
     *     TemplateExpression [6,19); TemplateHead [6,13) (span covers the
     *     leading space + `` `abc${ ``); TemplateSpan [13,19);
     *     NumericLiteral [13,14); TemplateTail [14,19) (`}abc` + closing `). */
    ctsc_arena_init(&a, 4096);
    src = "delete `abc${0}abc`;";
    int tpl_src_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)tpl_src_end, &a);
    EXPECT(tpl_src_end == 20);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    const CtscNode* tpl_stmt = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(tpl_stmt->kind == CTSC_SK_ExpressionStatement);
    EXPECT(tpl_stmt->pos == 0 && tpl_stmt->end == 20);
    const CtscNode* del = tpl_stmt->data.expressionStatement.expression;
    EXPECT(del != NULL);
    EXPECT(del->kind == CTSC_SK_DeleteExpression);
    EXPECT(del->pos == 0 && del->end == 19);
    const CtscNode* te = del->data.voidExpression.expression;
    EXPECT(te != NULL);
    EXPECT(te->kind == CTSC_SK_TemplateExpression);
    EXPECT(te->pos == 6 && te->end == 19);
    const CtscNode* head = te->data.templateExpression.head;
    EXPECT(head != NULL);
    EXPECT(head->kind == CTSC_SK_TemplateHead);
    EXPECT(head->pos == 6 && head->end == 13);
    EXPECT(te->data.templateExpression.templateSpans.len == 1);
    const CtscNode* span = te->data.templateExpression.templateSpans.items[0];
    EXPECT(span->kind == CTSC_SK_TemplateSpan);
    EXPECT(span->pos == 13 && span->end == 19);
    const CtscNode* span_expr = span->data.templateSpan.expression;
    EXPECT(span_expr != NULL);
    EXPECT(span_expr->kind == CTSC_SK_NumericLiteral);
    EXPECT(span_expr->pos == 13 && span_expr->end == 14);
    const CtscNode* tail = span->data.templateSpan.literal;
    EXPECT(tail != NULL);
    EXPECT(tail->kind == CTSC_SK_TemplateTail);
    EXPECT(tail->pos == 14 && tail->end == 19);
    ctsc_arena_free(&a);

    /* Template with two substitutions exercises the TemplateMiddle path
     * (parseTemplateSpans loop condition ~3664: continue while
     * literal.kind === TemplateMiddle). `` `a${1}b${2}c` `` has one Middle
     * and one Tail. */
    ctsc_arena_init(&a, 4096);
    src = "`a${1}b${2}c`;";
    int tpl2_src_end = (int)strlen(src);
    r = ctsc_parse(src, (size_t)tpl2_src_end, &a);
    EXPECT(tpl2_src_end == 14);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    const CtscNode* tpl2_stmt = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(tpl2_stmt->kind == CTSC_SK_ExpressionStatement);
    const CtscNode* te2 = tpl2_stmt->data.expressionStatement.expression;
    EXPECT(te2 != NULL);
    EXPECT(te2->kind == CTSC_SK_TemplateExpression);
    EXPECT(te2->pos == 0 && te2->end == 13);
    EXPECT(te2->data.templateExpression.head->kind == CTSC_SK_TemplateHead);
    EXPECT(te2->data.templateExpression.templateSpans.len == 2);
    const CtscNode* span2_0 = te2->data.templateExpression.templateSpans.items[0];
    EXPECT(span2_0->data.templateSpan.literal->kind == CTSC_SK_TemplateMiddle);
    const CtscNode* span2_1 = te2->data.templateExpression.templateSpans.items[1];
    EXPECT(span2_1->data.templateSpan.literal->kind == CTSC_SK_TemplateTail);
    ctsc_arena_free(&a);

    /* parser.ts parseBinaryExpressionRest (~5649): `as` Type yields AsExpression. */
    ctsc_arena_init(&a, 4096);
    src = "x as unknown;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 1);
    s = r.sourceFile->data.sourceFile.statements.items[0];
    EXPECT(s->kind == CTSC_SK_ExpressionStatement);
    EXPECT(s->data.expressionStatement.expression->kind == CTSC_SK_AsExpression);
    EXPECT(s->data.expressionStatement.expression->data.asExpression.expression->kind == CTSC_SK_Identifier);
    EXPECT(s->data.expressionStatement.expression->data.asExpression.type != NULL);
    ctsc_arena_free(&a);

    /*
     * LiteralType / keyword type in type-argument position. Mirrors upstream
     * parser.ts parseTypeArgumentsOfTypeReference (~3791) → parseType →
     * parseLiteralTypeNode (~4528) / parseNonArrayType keyword cases
     * (~4591-4612). ctsc's try_parse_type_argument_list previously collapsed
     * NumericLiteral / TrueKeyword / keyword-type arguments into an opaque
     * CTSC_SK_TypeReference via the stop-set fallback scan, so the checker
     * could not recover the literal/intrinsic type for a generic alias
     * instantiation like `Classify<42>` / `Classify<true>` / `Classify<number>`
     * (checker/conditional/05_cond_nested.ts).
     */
    ctsc_arena_init(&a, 4096);
    src = "type A<T> = T;\ntype B = A<42>;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 2);
    s = r.sourceFile->data.sourceFile.statements.items[1];
    EXPECT(s->kind == CTSC_SK_TypeAliasDeclaration);
    {
        const CtscNode* alias_ty = s->data.typeAliasDeclaration.type;
        EXPECT(alias_ty != NULL);
        EXPECT(alias_ty->kind == CTSC_SK_TypeReference);
        EXPECT(alias_ty->data.typeReference.has_type_arguments);
        EXPECT(alias_ty->data.typeReference.type_arguments.len == 1);
        const CtscNode* targ0 = alias_ty->data.typeReference.type_arguments.items[0];
        EXPECT(targ0 != NULL);
        EXPECT(targ0->kind == CTSC_SK_NumericLiteral);
    }
    ctsc_arena_free(&a);

    ctsc_arena_init(&a, 4096);
    src = "type A<T> = T;\ntype B = A<true>;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 2);
    s = r.sourceFile->data.sourceFile.statements.items[1];
    EXPECT(s->kind == CTSC_SK_TypeAliasDeclaration);
    {
        const CtscNode* alias_ty = s->data.typeAliasDeclaration.type;
        EXPECT(alias_ty != NULL);
        EXPECT(alias_ty->kind == CTSC_SK_TypeReference);
        EXPECT(alias_ty->data.typeReference.has_type_arguments);
        EXPECT(alias_ty->data.typeReference.type_arguments.len == 1);
        const CtscNode* targ0 = alias_ty->data.typeReference.type_arguments.items[0];
        EXPECT(targ0 != NULL);
        EXPECT(targ0->kind == CTSC_SK_TrueKeyword);
    }
    ctsc_arena_free(&a);

    ctsc_arena_init(&a, 4096);
    src = "type A<T> = T;\ntype B = A<number>;";
    r = ctsc_parse(src, strlen(src), &a);
    EXPECT(r.sourceFile->data.sourceFile.statements.len == 2);
    s = r.sourceFile->data.sourceFile.statements.items[1];
    EXPECT(s->kind == CTSC_SK_TypeAliasDeclaration);
    {
        const CtscNode* alias_ty = s->data.typeAliasDeclaration.type;
        EXPECT(alias_ty != NULL);
        EXPECT(alias_ty->kind == CTSC_SK_TypeReference);
        EXPECT(alias_ty->data.typeReference.has_type_arguments);
        EXPECT(alias_ty->data.typeReference.type_arguments.len == 1);
        const CtscNode* targ0 = alias_ty->data.typeReference.type_arguments.items[0];
        EXPECT(targ0 != NULL);
        EXPECT(targ0->kind == CTSC_SK_NumberKeyword);
    }
    ctsc_arena_free(&a);

    return failed;
}
