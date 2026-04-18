#include "ctsc/parser.h"
#include "ctsc/ast.h"
#include "ctsc/json_writer.h"
#include "ctsc/scanner.h"

static void emit_node(CtscJson* j, const CtscNode* n);

static void emit_array(CtscJson* j, const char* key, const CtscNodeArray* arr) {
    ctsc_json_key(j, key);
    ctsc_json_begin_arr(j);
    for (size_t i = 0; i < arr->len; ++i) emit_node(j, arr->items[i]);
    ctsc_json_end_arr(j);
}

/* Operator token as a child {kind,pos,end} matching tsc. */
static void emit_operator_token(CtscJson* j, const char* key, CtscSyntaxKind k, int pos, int end) {
    ctsc_json_key(j, key);
    ctsc_json_begin_obj(j);
    ctsc_json_key(j, "kind"); ctsc_json_cstr(j, ctsc_syntax_kind_name(k));
    ctsc_json_key(j, "pos");  ctsc_json_int(j, pos);
    ctsc_json_key(j, "end");  ctsc_json_int(j, end);
    ctsc_json_end_obj(j);
}

static void emit_node(CtscJson* j, const CtscNode* n) {
    if (!n) { ctsc_json_null(j); return; }
    ctsc_json_begin_obj(j);
    ctsc_json_key(j, "kind"); ctsc_json_cstr(j, ctsc_syntax_kind_name(n->kind));
    ctsc_json_key(j, "pos");  ctsc_json_int(j, n->pos);
    ctsc_json_key(j, "end");  ctsc_json_int(j, n->end);
    switch (n->kind) {
        case CTSC_SK_SourceFile:
            emit_array(j, "statements", &n->data.sourceFile.statements);
            break;
        case CTSC_SK_Block:
            emit_array(j, "statements", &n->data.block.statements);
            break;
        case CTSC_SK_Identifier:
            ctsc_json_key(j, "escapedText");
            ctsc_json_str_utf16(j, n->data.identifier.text, n->data.identifier.text_len);
            break;
        case CTSC_SK_NumericLiteral:
            ctsc_json_key(j, "text");
            ctsc_json_str_utf16(j, n->data.numericLiteral.text, n->data.numericLiteral.text_len);
            break;
        case CTSC_SK_StringLiteral:
            ctsc_json_key(j, "text");
            ctsc_json_str_utf16(j, n->data.stringLiteral.value, n->data.stringLiteral.value_len);
            if (n->data.stringLiteral.single_quote) {
                ctsc_json_key(j, "singleQuote"); ctsc_json_bool(j, true);
            }
            break;
        case CTSC_SK_RegularExpressionLiteral:
            /* tsc's RegularExpressionLiteral carries `text`, but our oracle
             * (harness/src/oracle-ast.ts) omits it for this kind, so we stay
             * silent to keep byte-exact parity. */
            break;
        case CTSC_SK_ExpressionStatement:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.expressionStatement.expression);
            break;
        case CTSC_SK_EmptyStatement:
            break;
        case CTSC_SK_DebuggerStatement:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts parseDebuggerStatement
             * (~7115): DebuggerStatement carries no payload children
             * (forEachChild visits nothing). The oracle (harness/src/oracle-ast.ts)
             * falls through to its default branch which emits `children` only when
             * non-empty, so {kind,pos,end} alone matches tsc byte-for-byte. */
            break;
        case CTSC_SK_ReturnStatement:
            if (n->data.returnStatement.expression) {
                ctsc_json_key(j, "expression");
                emit_node(j, n->data.returnStatement.expression);
            }
            break;
        case CTSC_SK_BreakStatement:
        case CTSC_SK_ContinueStatement:
            /* tsc's forEachChild visits only the label when present, and the
             * oracle's default branch serializes a single `children` array.
             * We mirror that: emit `children:[label]` when label is present,
             * otherwise emit no extra fields (matches {"kind","pos","end"}
             * parity for labeless break;/continue;). */
            if (n->data.breakOrContinueStatement.label) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                emit_node(j, n->data.breakOrContinueStatement.label);
                ctsc_json_end_arr(j);
            }
            break;
        case CTSC_SK_VariableStatement:
            ctsc_json_key(j, "declarationList");
            emit_node(j, n->data.variableStatement.declarationList);
            break;
        case CTSC_SK_VariableDeclarationList: {
            emit_array(j, "declarations", &n->data.variableDeclarationList.declarations);
            /* flags are internal; tsc serializes via getCombinedNodeFlags — we omit. */
            break;
        }
        case CTSC_SK_VariableDeclaration:
            ctsc_json_key(j, "name");
            emit_node(j, n->data.variableDeclaration.name);
            if (n->data.variableDeclaration.initializer) {
                ctsc_json_key(j, "initializer");
                emit_node(j, n->data.variableDeclaration.initializer);
            }
            break;
        case CTSC_SK_BinaryExpression: {
            ctsc_json_key(j, "left");
            emit_node(j, n->data.binaryExpression.left);
            /* tsc uses `operatorToken` with {kind,pos,end}; we approximate
             * pos/end by spanning the gap between left.end and right.pos */
            emit_operator_token(j, "operatorToken",
                n->data.binaryExpression.operator_kind,
                n->data.binaryExpression.left->end,
                n->data.binaryExpression.right->pos);
            ctsc_json_key(j, "right");
            emit_node(j, n->data.binaryExpression.right);
            break;
        }
        case CTSC_SK_CallExpression:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.callExpression.expression);
            emit_array(j, "arguments", &n->data.callExpression.arguments);
            break;
        case CTSC_SK_NewExpression: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInCallOrNewExpression (~1158): visits expression,
             * questionDotToken, typeArguments, arguments. The oracle
             * (harness/src/oracle-ast.ts) has no explicit case for
             * NewExpression, so it falls through to the default branch which
             * serializes forEachChild's visits as a single `children` array
             * (and only when non-empty). ctsc does not yet model
             * questionDotToken or typeArguments on NewExpression; see the
             * 105_parserConstructorAmbiguity2.ts fixture where tsc rolls the
             * `<...` type-argument try-parse back, leaving just the
             * expression in children. */
            size_t child_count = 1 /* expression */
                + (n->data.newExpression.has_arguments ? n->data.newExpression.arguments.len : 0);
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                emit_node(j, n->data.newExpression.expression);
                if (n->data.newExpression.has_arguments) {
                    for (size_t i = 0; i < n->data.newExpression.arguments.len; ++i) {
                        emit_node(j, n->data.newExpression.arguments.items[i]);
                    }
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_FunctionDeclaration:
            if (n->data.functionDeclaration.name) {
                ctsc_json_key(j, "name");
                emit_node(j, n->data.functionDeclaration.name);
            }
            emit_array(j, "parameters", &n->data.functionDeclaration.parameters);
            if (n->data.functionDeclaration.body) {
                ctsc_json_key(j, "body");
                emit_node(j, n->data.functionDeclaration.body);
            }
            break;
        case CTSC_SK_Parameter:
            ctsc_json_key(j, "name");
            emit_node(j, n->data.parameter.name);
            break;
        case CTSC_SK_IfStatement:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.ifStatement.expression);
            ctsc_json_key(j, "thenStatement");
            emit_node(j, n->data.ifStatement.thenStatement);
            if (n->data.ifStatement.elseStatement) {
                ctsc_json_key(j, "elseStatement");
                emit_node(j, n->data.ifStatement.elseStatement);
            }
            break;
        case CTSC_SK_WhileStatement:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.whileStatement.expression);
            ctsc_json_key(j, "statement");
            emit_node(j, n->data.whileStatement.statement);
            break;
        case CTSC_SK_ForStatement:
            if (n->data.forStatement.initializer) {
                ctsc_json_key(j, "initializer");
                emit_node(j, n->data.forStatement.initializer);
            }
            if (n->data.forStatement.condition) {
                ctsc_json_key(j, "condition");
                emit_node(j, n->data.forStatement.condition);
            }
            if (n->data.forStatement.incrementor) {
                ctsc_json_key(j, "incrementor");
                emit_node(j, n->data.forStatement.incrementor);
            }
            ctsc_json_key(j, "statement");
            emit_node(j, n->data.forStatement.statement);
            break;
        case CTSC_SK_ParenthesizedExpression:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.parenthesizedExpression.expression);
            break;
        case CTSC_SK_PrefixUnaryExpression:
            /* tsc serializes operator as numeric SyntaxKind; we use the
             * canonical string name so the oracle and ctsc can agree without
             * sharing enum values. */
            ctsc_json_key(j, "operator");
            ctsc_json_cstr(j, ctsc_syntax_kind_name(n->data.prefixUnaryExpression.operator_kind));
            ctsc_json_key(j, "operand");
            emit_node(j, n->data.prefixUnaryExpression.operand);
            break;
        case CTSC_SK_PostfixUnaryExpression:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInPostfixUnaryExpression (~780): visits only the
             * operand. The oracle's default branch serializes that as a
             * single-element `children` array, and does NOT emit `operator`.
             * See harness/oracle fixture parserStrictMode6-negative.ts. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.postfixUnaryExpression.operand);
            ctsc_json_end_arr(j);
            break;
        case CTSC_SK_ArrayLiteralExpression:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInArrayLiteralExpression (~1936, see typescript.js
             * [ArrayLiteralExpression]): visits only elements. The oracle's
             * default branch serializes those visits as a single `children`
             * array and does NOT emit `elements`. Only emit `children` when
             * there is at least one element, matching
             * `if (children.length) o.children = children;` in oracle-ast.ts. */
            if (n->data.arrayLiteralExpression.elements.len > 0) {
                emit_array(j, "children", &n->data.arrayLiteralExpression.elements);
            }
            break;
        case CTSC_SK_PropertyAccessExpression:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.propertyAccessExpression.expression);
            ctsc_json_key(j, "name");
            emit_node(j, n->data.propertyAccessExpression.name);
            break;
        case CTSC_SK_ElementAccessExpression:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInElementAccessExpression (~1162): visits
             * expression, questionDotToken, argumentExpression. The oracle
             * (harness/src/oracle-ast.ts) has no explicit case so it falls
             * through to the default branch and emits forEachChild's visits
             * as a single `children` array. ctsc does not yet model
             * questionDotToken, so the two-element shape
             * `children:[expression, argumentExpression]` matches tsc's
             * non-optional element access byte-for-byte (see the
             * 105_parserObjectCreationArrayLiteral1.ts oracle:
             * children:[Identifier "Foo", Identifier ""]). */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.elementAccessExpression.expression);
            emit_node(j, n->data.elementAccessExpression.argumentExpression);
            ctsc_json_end_arr(j);
            break;
        case CTSC_SK_ConditionalExpression:
            ctsc_json_key(j, "condition");
            emit_node(j, n->data.conditionalExpression.condition);
            ctsc_json_key(j, "whenTrue");
            emit_node(j, n->data.conditionalExpression.whenTrue);
            ctsc_json_key(j, "whenFalse");
            emit_node(j, n->data.conditionalExpression.whenFalse);
            break;
        case CTSC_SK_VoidExpression:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInVoidExpression (~767): visits only `expression`.
             * The oracle (harness/src/oracle-ast.ts) has no explicit case for
             * VoidExpression, so it falls through to the default branch which
             * serializes forEachChild's visits as a `children` array. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.voidExpression.expression);
            ctsc_json_end_arr(j);
            break;
        case CTSC_SK_YieldExpression: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInYieldExpression (~773): visits asteriskToken then
             * expression. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for YieldExpression, so it falls through to the
             * default branch which serializes forEachChild's visits as a
             * single `children` array (and only when non-empty). Undefined
             * children are skipped by ts.forEachChild / visitNode, so bare
             * `yield` emits `{kind,pos,end}` alone and `yield foo` emits
             * `children:[<foo>]`. */
            size_t child_count = (n->data.yieldExpression.has_asterisk ? 1 : 0)
                + (n->data.yieldExpression.expression ? 1 : 0);
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.yieldExpression.has_asterisk) {
                    /* AsteriskToken leaf: {kind,pos,end}. Mirrors tsc's
                     * factoryCreateToken(SyntaxKind.AsteriskToken) with
                     * finishNode positions recorded during parse. */
                    ctsc_json_begin_obj(j);
                    ctsc_json_key(j, "kind"); ctsc_json_cstr(j, ctsc_syntax_kind_name(CTSC_SK_AsteriskToken));
                    ctsc_json_key(j, "pos");  ctsc_json_int(j, n->data.yieldExpression.asterisk_pos);
                    ctsc_json_key(j, "end");  ctsc_json_int(j, n->data.yieldExpression.asterisk_end);
                    ctsc_json_end_obj(j);
                }
                if (n->data.yieldExpression.expression) {
                    emit_node(j, n->data.yieldExpression.expression);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        default:
            break;
    }
    ctsc_json_end_obj(j);
}

void ctsc_ast_dump_json(const CtscNode* sourceFile, CtscBuffer* out, bool pretty) {
    CtscJson j; ctsc_json_init(&j, out, pretty);
    emit_node(&j, sourceFile);
}
