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
        case CTSC_SK_PrivateIdentifier:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * parsePrivateIdentifier (~2747): a leaf node whose forEachChild
             * visits nothing. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for PrivateIdentifier, so it falls through to
             * the default branch which emits only `{kind,pos,end}` (no
             * `escapedText` because PrivateIdentifier is not a
             * ts.Identifier, and `children` is empty so the default branch
             * omits that key too). We mirror that explicitly so the field
             * list stays stable even if CtscIdentifierData grows. */
            break;
        case CTSC_SK_NumericLiteral:
            ctsc_json_key(j, "text");
            ctsc_json_str_utf16(j, n->data.numericLiteral.text, n->data.numericLiteral.text_len);
            break;
        case CTSC_SK_BigIntLiteral:
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
        case CTSC_SK_ThrowStatement:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.throwStatement.expression);
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
        case CTSC_SK_LabeledStatement:
            /* forEachChildInLabeledStatement (~880): label, statement. */
            ctsc_json_key(j, "label");
            emit_node(j, n->data.labeledStatement.label);
            if (n->data.labeledStatement.statement) {
                ctsc_json_key(j, "statement");
                emit_node(j, n->data.labeledStatement.statement);
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
             * questionDotToken; typeArguments are absorbed from the
             * speculative `<...>` try-parse (see parser.c parse_new_expression)
             * — the 105_parserConstructorAmbiguity2.ts fixture rolls the
             * try-parse back when `<A` fails to close, leaving just the
             * expression; the 106_parserConstructorAmbiguity3.ts fixture
             * (`new Date<A>` at EOF) keeps them. */
            size_t child_count = 1 /* expression */
                + (n->data.newExpression.has_type_arguments ? n->data.newExpression.type_arguments.len : 0)
                + (n->data.newExpression.has_arguments ? n->data.newExpression.arguments.len : 0);
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                emit_node(j, n->data.newExpression.expression);
                if (n->data.newExpression.has_type_arguments) {
                    for (size_t i = 0; i < n->data.newExpression.type_arguments.len; ++i) {
                        emit_node(j, n->data.newExpression.type_arguments.items[i]);
                    }
                }
                if (n->data.newExpression.has_arguments) {
                    for (size_t i = 0; i < n->data.newExpression.arguments.len; ++i) {
                        emit_node(j, n->data.newExpression.arguments.items[i]);
                    }
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_TypeReference: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInTypeReference: visits typeName then each
             * typeArgument. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for TypeReference, so it falls through to the
             * default branch which serializes forEachChild's visits as a
             * single `children` array (and only when non-empty). When the
             * parser's fallback path (non-identifier type atoms) produced a
             * TypeReference with no typeName, we still emit `{kind,pos,end}`
             * only — nothing serializes to a `children` array in that case. */
            size_t child_count = 0;
            if (n->data.typeReference.typeName) child_count++;
            if (n->data.typeReference.has_type_arguments) {
                child_count += n->data.typeReference.type_arguments.len;
            }
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.typeReference.typeName) {
                    emit_node(j, n->data.typeReference.typeName);
                }
                if (n->data.typeReference.has_type_arguments) {
                    for (size_t i = 0; i < n->data.typeReference.type_arguments.len; ++i) {
                        emit_node(j, n->data.typeReference.type_arguments.items[i]);
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
        case CTSC_SK_FunctionExpression: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInFunctionLikeDeclaration (~548): visits modifiers,
             * asteriskToken, name, questionToken, exclamationToken,
             * typeParameters, parameters, type, body (in that order). The
             * oracle (harness/src/oracle-ast.ts) has no explicit case for
             * FunctionExpression, so it falls through to the default branch
             * which serialises forEachChild's visits as a single `children`
             * array (only when non-empty). ctsc currently models
             * asteriskToken (for generator `function *`), name (optional),
             * parameters, and body — the rest are skipped until a fixture
             * demands them. Undefined children are skipped the way
             * ts.forEachChild / visitNode does. The AsteriskToken leaf is
             * required by fixture 108_FunctionExpression1_es6.ts
             * (`function * () { }`). */
            size_t child_count = (n->data.functionDeclaration.has_asterisk ? 1 : 0)
                + (n->data.functionDeclaration.name ? 1 : 0)
                + n->data.functionDeclaration.parameters.len
                + (n->data.functionDeclaration.body ? 1 : 0);
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.functionDeclaration.has_asterisk) {
                    /* AsteriskToken leaf: {kind,pos,end}. Mirrors tsc's
                     * factoryCreateToken(AsteriskToken) / parseTokenNode
                     * (~2553) with finishNode positions. */
                    ctsc_json_begin_obj(j);
                    ctsc_json_key(j, "kind");
                    ctsc_json_cstr(j, ctsc_syntax_kind_name(CTSC_SK_AsteriskToken));
                    ctsc_json_key(j, "pos");
                    ctsc_json_int(j, n->data.functionDeclaration.asterisk_pos);
                    ctsc_json_key(j, "end");
                    ctsc_json_int(j, n->data.functionDeclaration.asterisk_end);
                    ctsc_json_end_obj(j);
                }
                if (n->data.functionDeclaration.name) {
                    emit_node(j, n->data.functionDeclaration.name);
                }
                for (size_t i = 0; i < n->data.functionDeclaration.parameters.len; ++i) {
                    emit_node(j, n->data.functionDeclaration.parameters.items[i]);
                }
                if (n->data.functionDeclaration.body) {
                    emit_node(j, n->data.functionDeclaration.body);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
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
        case CTSC_SK_SwitchStatement:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.switchStatement.expression);
            ctsc_json_key(j, "caseBlock");
            emit_node(j, n->data.switchStatement.caseBlock);
            break;
        case CTSC_SK_CaseBlock:
            emit_array(j, "clauses", &n->data.caseBlock.clauses);
            break;
        case CTSC_SK_CaseClause:
            ctsc_json_key(j, "expression");
            emit_node(j, n->data.caseClause.expression);
            emit_array(j, "statements", &n->data.caseClause.statements);
            break;
        case CTSC_SK_DefaultClause:
            emit_array(j, "statements", &n->data.defaultClause.statements);
            break;
        case CTSC_SK_DoStatement: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInDoStatement (~832): visits statement then
             * expression. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for DoStatement, so it falls through to the
             * default branch which serialises those visits as a single
             * `children` array. Both children are always present. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.doStatement.statement);
            emit_node(j, n->data.doStatement.expression);
            ctsc_json_end_arr(j);
            break;
        }
        case CTSC_SK_WithStatement: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInWithStatement (~862): visits expression then
             * statement. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for WithStatement, so it falls through to the
             * default branch which serialises those visits as a single
             * `children` array. Both children are always present. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.withStatement.expression);
            emit_node(j, n->data.withStatement.statement);
            ctsc_json_end_arr(j);
            break;
        }
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
        case CTSC_SK_ForInStatement:
        case CTSC_SK_ForOfStatement: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInForInStatement (~846) and forEachChildInForOfStatement
             * (~851). The oracle (harness/src/oracle-ast.ts) has no explicit case
             * for either kind, so both fall through to the default branch which
             * serialises forEachChild's visits as a single `children` array
             * (only when non-empty). Order:
             *   ForIn: initializer, expression, statement
             *   ForOf: awaitModifier?, initializer, expression, statement
             * awaitModifier is not modelled yet (no active fixture). */
            size_t child_count = 0;
            if (n->data.forInOrOfStatement.initializer) child_count++;
            if (n->data.forInOrOfStatement.expression)  child_count++;
            if (n->data.forInOrOfStatement.statement)   child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.forInOrOfStatement.initializer) {
                    emit_node(j, n->data.forInOrOfStatement.initializer);
                }
                if (n->data.forInOrOfStatement.expression) {
                    emit_node(j, n->data.forInOrOfStatement.expression);
                }
                if (n->data.forInOrOfStatement.statement) {
                    emit_node(j, n->data.forInOrOfStatement.statement);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
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
        case CTSC_SK_ObjectLiteralExpression:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInObjectLiteralExpression: visits only `properties`.
             * The oracle (harness/src/oracle-ast.ts) has no explicit case for
             * ObjectLiteralExpression so it falls through to the default
             * branch which serializes forEachChild's visits as a single
             * `children` array (only when non-empty). Emitting the key only
             * when there is at least one property matches
             * `if (children.length) o.children = children;` in oracle-ast.ts.
             */
            if (n->data.objectLiteralExpression.properties.len > 0) {
                emit_array(j, "children", &n->data.objectLiteralExpression.properties);
            }
            break;
        case CTSC_SK_EnumMember:
        case CTSC_SK_PropertyAssignment: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInPropertyAssignment: visits modifiers, name,
             * questionToken, exclamationToken, initializer. The oracle has no
             * explicit case for PropertyAssignment so it falls through to the
             * default branch which serialises those visits as a single
             * `children` array. ctsc currently models `name` and
             * `initializer` only; both are always present (initializer is a
             * missing Identifier when the `:` recovery path fires, per
             * parser.c parse_object_literal_element). */
            size_t child_count = 0;
            if (n->data.propertyAssignment.name) child_count++;
            if (n->data.propertyAssignment.initializer) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.propertyAssignment.name) {
                    emit_node(j, n->data.propertyAssignment.name);
                }
                if (n->data.propertyAssignment.initializer) {
                    emit_node(j, n->data.propertyAssignment.initializer);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_ShorthandPropertyAssignment: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInShorthandPropertyAssignment: visits modifiers,
             * name, questionToken, exclamationToken, equalsToken,
             * objectAssignmentInitializer. The oracle default branch emits
             * `children`. ctsc currently models `name` and the optional
             * `objectAssignmentInitializer` only. */
            size_t child_count = 0;
            if (n->data.shorthandPropertyAssignment.name) child_count++;
            if (n->data.shorthandPropertyAssignment.objectAssignmentInitializer) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.shorthandPropertyAssignment.name) {
                    emit_node(j, n->data.shorthandPropertyAssignment.name);
                }
                if (n->data.shorthandPropertyAssignment.objectAssignmentInitializer) {
                    emit_node(j, n->data.shorthandPropertyAssignment.objectAssignmentInitializer);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_SpreadAssignment:
            /* forEachChildInSpreadAssignment: expression only. */
            if (n->data.spreadAssignment.expression) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                emit_node(j, n->data.spreadAssignment.expression);
                ctsc_json_end_arr(j);
            }
            break;
        case CTSC_SK_SpreadElement:
            /* forEachChildInSpreadElement: expression only (types.ts). */
            if (n->data.spreadElement.expression) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                emit_node(j, n->data.spreadElement.expression);
                ctsc_json_end_arr(j);
            }
            break;
        case CTSC_SK_ComputedPropertyName:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInComputedPropertyName: visits only `expression`.
             * The oracle default branch emits a single `children` array; we
             * always have an expression (missing Identifier fallback when the
             * `[ ]` slot is empty). */
            if (n->data.computedPropertyName.expression) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                emit_node(j, n->data.computedPropertyName.expression);
                ctsc_json_end_arr(j);
            }
            break;
        case CTSC_SK_ClassDeclaration:
        case CTSC_SK_ClassExpression:
        case CTSC_SK_InterfaceDeclaration: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInClassDeclarationOrExpression (~1174) and
             * forEachChildInInterfaceDeclaration (~901): both visit
             *   modifiers, name, typeParameters, heritageClauses, members
             * in that order. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for either kind so they fall through to the
             * default branch which serializes forEachChild's visits as a
             * single `children` array (only when non-empty). ClassDeclaration,
             * ClassExpression, and InterfaceDeclaration share the same data
             * struct (CtscClassDeclarationData) and emission — parser.ts
             * factory branches diverge only on the node kind string, so we
             * switch on the node kind here.
             *
             * ctsc now models `modifiers`, `name`, `typeParameters`,
             * `heritageClauses`, and `members`. Undefined / empty NodeArrays
             * are skipped the way ts.forEachChild / visitNodes skip them,
             * matching tsc.
             *
             * Visit order matches forEachChildInClassDeclarationOrExpression:
             * modifiers → name → typeParameters → heritageClauses → members.
             * For `class C<T> {}` (fixtures/parser/from-upstream/107_parserGenericClass1.ts)
             * the expected `children` array is `[Identifier(C), TypeParameter(T)]`.
             * For `interface I extends { }`
             * (108_parserErrorRecovery_ExtendsOrImplementsClause6.ts) it is
             * `[Identifier(I), HeritageClause]`. For
             * `protected class C {}` (108_Protected1.ts) it is
             * `[ProtectedKeyword, Identifier(C)]` — the ProtectedKeyword
             * leaf comes from parseModifiers before parseClassDeclaration
             * absorbs the `class` keyword. */
            size_t child_count = 0;
            child_count += n->data.classDeclaration.modifiers.len;
            if (n->data.classDeclaration.name) child_count++;
            child_count += n->data.classDeclaration.type_parameters.len;
            child_count += n->data.classDeclaration.heritage_clauses.len;
            child_count += n->data.classDeclaration.members.len;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                for (size_t i = 0; i < n->data.classDeclaration.modifiers.len; ++i) {
                    emit_node(j, n->data.classDeclaration.modifiers.items[i]);
                }
                if (n->data.classDeclaration.name) {
                    emit_node(j, n->data.classDeclaration.name);
                }
                for (size_t i = 0; i < n->data.classDeclaration.type_parameters.len; ++i) {
                    emit_node(j, n->data.classDeclaration.type_parameters.items[i]);
                }
                for (size_t i = 0; i < n->data.classDeclaration.heritage_clauses.len; ++i) {
                    emit_node(j, n->data.classDeclaration.heritage_clauses.items[i]);
                }
                for (size_t i = 0; i < n->data.classDeclaration.members.len; ++i) {
                    emit_node(j, n->data.classDeclaration.members.items[i]);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_TypeAliasDeclaration: {
            /* Mirrors upstream parser.ts forEachChildInTypeAliasDeclaration (~907):
             * modifiers, name, typeParameters, type. */
            size_t child_count = n->data.typeAliasDeclaration.modifiers.len;
            if (n->data.typeAliasDeclaration.name) child_count++;
            child_count += n->data.typeAliasDeclaration.type_parameters.len;
            if (n->data.typeAliasDeclaration.type) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                for (size_t i = 0; i < n->data.typeAliasDeclaration.modifiers.len; ++i) {
                    emit_node(j, n->data.typeAliasDeclaration.modifiers.items[i]);
                }
                if (n->data.typeAliasDeclaration.name) {
                    emit_node(j, n->data.typeAliasDeclaration.name);
                }
                for (size_t i = 0; i < n->data.typeAliasDeclaration.type_parameters.len; ++i) {
                    emit_node(j, n->data.typeAliasDeclaration.type_parameters.items[i]);
                }
                if (n->data.typeAliasDeclaration.type) {
                    emit_node(j, n->data.typeAliasDeclaration.type);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_HeritageClause: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInHeritageClause (~633): visits each element of
             * `types`. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for HeritageClause so it falls through to the
             * default branch, which serialises forEachChild's visits as a
             * single `children` array (emitted only when non-empty). tsc's
             * HeritageClause also carries `token` (the ExtendsKeyword /
             * ImplementsKeyword kind), but the oracle's default branch
             * does NOT surface it — only enumerable properties that are
             * Node-shaped get recursed. For the
             * 108_parserErrorRecovery_ExtendsOrImplementsClause1.ts fixture
             * (`class C extends {\n}`) the empty-types branch fires and we
             * emit only {kind,pos,end}, matching the oracle byte-for-byte. */
            if (n->data.heritageClause.types.len > 0) {
                emit_array(j, "children", &n->data.heritageClause.types);
            }
            break;
        }
        case CTSC_SK_ExpressionWithTypeArguments: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInExpressionWithTypeArguments: visits expression
             * then each typeArgument. The oracle (harness/src/oracle-ast.ts)
             * has no explicit case so it falls through to the default
             * branch which serialises forEachChild's visits as a single
             * `children` array (emitted only when non-empty). */
            size_t child_count = 0;
            if (n->data.expressionWithTypeArguments.expression) child_count++;
            if (n->data.expressionWithTypeArguments.has_type_arguments) {
                child_count += n->data.expressionWithTypeArguments.type_arguments.len;
            }
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.expressionWithTypeArguments.expression) {
                    emit_node(j, n->data.expressionWithTypeArguments.expression);
                }
                if (n->data.expressionWithTypeArguments.has_type_arguments) {
                    for (size_t i = 0; i < n->data.expressionWithTypeArguments.type_arguments.len; ++i) {
                        emit_node(j, n->data.expressionWithTypeArguments.type_arguments.items[i]);
                    }
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_EnumDeclaration: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInEnumDeclaration: visits modifiers, name, members
             * in that order. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for EnumDeclaration so it falls through to the
             * default branch which serialises forEachChild's visits as a
             * single `children` array (only when non-empty). ctsc currently
             * models only `name` and `members`; modifiers are skipped until
             * a fixture demands them. `name` is always present (zero-width
             * missing Identifier when the source puts a reserved word after
             * `enum`, e.g. `enum void {}` in 106_parserEnumDeclaration4.ts).
             */
            size_t child_count = 0;
            if (n->data.enumDeclaration.name) child_count++;
            child_count += n->data.enumDeclaration.members.len;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.enumDeclaration.name) {
                    emit_node(j, n->data.enumDeclaration.name);
                }
                for (size_t i = 0; i < n->data.enumDeclaration.members.len; ++i) {
                    emit_node(j, n->data.enumDeclaration.members.items[i]);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_MethodDeclaration: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInMethodDeclaration: visits modifiers, asteriskToken,
             * name, questionToken, exclamationToken, typeParameters, parameters,
             * type, body (in that order). The oracle (harness/src/oracle-ast.ts)
             * has no explicit case for MethodDeclaration so it falls through
             * to the default branch which serializes forEachChild's visits as
             * a single `children` array (only when non-empty).
             *
             * ctsc models: modifiers, asteriskToken, name, typeParameters,
             * parameters, optional return `type`, body. Undefined visits are skipped
             * the way ts.forEachChild / visitNode skip undefined, so the
             * `{kind,pos,end}` only shape is emitted when all four are absent
             * (currently impossible because name is always at least the zero-
             * width missing Identifier).
             */
            size_t child_count = n->data.methodDeclaration.modifiers.len;
            if (n->data.methodDeclaration.has_asterisk) child_count++;
            if (n->data.methodDeclaration.name) child_count++;
            child_count += n->data.methodDeclaration.type_parameters.len;
            child_count += n->data.methodDeclaration.parameters.len;
            if (n->data.methodDeclaration.type) child_count++;
            if (n->data.methodDeclaration.body) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                for (size_t mi = 0; mi < n->data.methodDeclaration.modifiers.len; ++mi) {
                    emit_node(j, n->data.methodDeclaration.modifiers.items[mi]);
                }
                if (n->data.methodDeclaration.has_asterisk) {
                    /* AsteriskToken leaf: {kind,pos,end}. Mirrors tsc's
                     * factoryCreateToken(AsteriskToken) / parseTokenNode
                     * (~2553) with finishNode positions. */
                    ctsc_json_begin_obj(j);
                    ctsc_json_key(j, "kind");
                    ctsc_json_cstr(j, ctsc_syntax_kind_name(CTSC_SK_AsteriskToken));
                    ctsc_json_key(j, "pos");
                    ctsc_json_int(j, n->data.methodDeclaration.asterisk_pos);
                    ctsc_json_key(j, "end");
                    ctsc_json_int(j, n->data.methodDeclaration.asterisk_end);
                    ctsc_json_end_obj(j);
                }
                if (n->data.methodDeclaration.name) {
                    emit_node(j, n->data.methodDeclaration.name);
                }
                for (size_t i = 0; i < n->data.methodDeclaration.type_parameters.len; ++i) {
                    emit_node(j, n->data.methodDeclaration.type_parameters.items[i]);
                }
                for (size_t i = 0; i < n->data.methodDeclaration.parameters.len; ++i) {
                    emit_node(j, n->data.methodDeclaration.parameters.items[i]);
                }
                if (n->data.methodDeclaration.type) {
                    emit_node(j, n->data.methodDeclaration.type);
                }
                if (n->data.methodDeclaration.body) {
                    emit_node(j, n->data.methodDeclaration.body);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_GetAccessor:
        case CTSC_SK_SetAccessor: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInGetAccessor (~617) / forEachChildInSetAccessor
             * (~625): visits modifiers, name, typeParameters, parameters,
             * type, body (in that order). The oracle
             * (harness/src/oracle-ast.ts) has no explicit case for
             * GetAccessor / SetAccessor so it falls through to the default
             * branch which serializes forEachChild's visits as a single
             * `children` array (only when non-empty).
             *
             * ctsc emits modifiers, name, parameters, optional `type` (getter
             * return annotation), then body — matching forEachChild order for
             * the fields we model (typeParameters remain unserialized until needed).
             * Undefined visits are skipped the way ts.forEachChild / visitNode skip
             * undefined. */
            size_t child_count = n->data.accessorDeclaration.modifiers.len;
            if (n->data.accessorDeclaration.name) child_count++;
            child_count += n->data.accessorDeclaration.parameters.len;
            if (n->data.accessorDeclaration.type) child_count++;
            if (n->data.accessorDeclaration.body) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                for (size_t mi = 0; mi < n->data.accessorDeclaration.modifiers.len; ++mi) {
                    emit_node(j, n->data.accessorDeclaration.modifiers.items[mi]);
                }
                if (n->data.accessorDeclaration.name) {
                    emit_node(j, n->data.accessorDeclaration.name);
                }
                for (size_t i = 0; i < n->data.accessorDeclaration.parameters.len; ++i) {
                    emit_node(j, n->data.accessorDeclaration.parameters.items[i]);
                }
                if (n->data.accessorDeclaration.type) {
                    emit_node(j, n->data.accessorDeclaration.type);
                }
                if (n->data.accessorDeclaration.body) {
                    emit_node(j, n->data.accessorDeclaration.body);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_PropertyDeclaration: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInPropertyDeclaration (~536): visits modifiers,
             * name, questionToken, exclamationToken, type, initializer
             * (in that order). The oracle (harness/src/oracle-ast.ts) has
             * no explicit case for PropertyDeclaration so it falls through
             * to the default branch which serializes forEachChild's visits
             * as a single `children` array (only when non-empty).
             *
             * ctsc currently models `name`, `type`, and `initializer`;
             * modifiers, questionToken, and exclamationToken are skipped
             * until a fixture demands them. Unlocks:
             *   - 108_parserComputedPropertyName9.ts: `class C { [e]: Type }`
             *     → children=[ComputedPropertyName, TypeReference]
             *   - 108_parserComputedPropertyName10.ts: `class C { [e] = 1 }`
             *     → children=[ComputedPropertyName, NumericLiteral]
             * Undefined visits are skipped the way ts.forEachChild /
             * visitNode skip undefined. */
            size_t child_count = n->data.propertyDeclaration.modifiers.len;
            if (n->data.propertyDeclaration.name) child_count++;
            if (n->data.propertyDeclaration.type) child_count++;
            if (n->data.propertyDeclaration.initializer) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                for (size_t mi = 0; mi < n->data.propertyDeclaration.modifiers.len; ++mi) {
                    emit_node(j, n->data.propertyDeclaration.modifiers.items[mi]);
                }
                if (n->data.propertyDeclaration.name) {
                    emit_node(j, n->data.propertyDeclaration.name);
                }
                if (n->data.propertyDeclaration.type) {
                    emit_node(j, n->data.propertyDeclaration.type);
                }
                if (n->data.propertyDeclaration.initializer) {
                    emit_node(j, n->data.propertyDeclaration.initializer);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_TypeParameter: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInTypeParameter (~510): visits modifiers, name,
             * constraint, default, expression (in that order). The oracle
             * (harness/src/oracle-ast.ts) has no explicit case for
             * TypeParameter, so it falls through to the default branch
             * which serialises forEachChild's visits as a single `children`
             * array. ctsc models `name` and `constraint`; modifiers /
             * default / expression are skipped until a fixture requires them. */
            if (n->data.typeParameter.name || n->data.typeParameter.constraint) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.typeParameter.name) {
                    emit_node(j, n->data.typeParameter.name);
                }
                if (n->data.typeParameter.constraint) {
                    emit_node(j, n->data.typeParameter.constraint);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
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
        case CTSC_SK_DeleteExpression:
        case CTSC_SK_TypeOfExpression:
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInVoidExpression (~767), forEachChildInDeleteExpression
             * (~763) and forEachChildInTypeOfExpression (~765): each visits
             * only `expression`. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for any of them, so they all fall through to the
             * default branch which serializes forEachChild's visits as a
             * `children` array. ctsc stores the three kinds in a single shared
             * voidExpression data struct (see ast.h ~336). */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.voidExpression.expression);
            ctsc_json_end_arr(j);
            break;
        case CTSC_SK_AwaitExpression:
            /* forEachChildInAwaitExpression (~751): visits `expression`. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.awaitExpression.expression);
            ctsc_json_end_arr(j);
            break;
        case CTSC_SK_TypeAssertionExpression: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInTypeAssertionExpression (~758): visits `type` then
             * `expression`. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for TypeAssertionExpression, so it falls through
             * to the default branch which serialises those visits as a single
             * `children` array. Both children are always present (parseType
             * and parseSimpleUnaryExpression both produce a node — the latter
             * falls back to a zero-width missing Identifier on EOF, the former
             * is driven through the keyword-type / parse_type_node path here). */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.typeAssertionExpression.type);
            emit_node(j, n->data.typeAssertionExpression.expression);
            ctsc_json_end_arr(j);
            break;
        }
        case CTSC_SK_AsExpression:
            /* forEachChildInAsExpression (~788): expression, then type. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.asExpression.expression);
            emit_node(j, n->data.asExpression.type);
            ctsc_json_end_arr(j);
            break;
        case CTSC_SK_TryStatement: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInTryStatement (~890): visits tryBlock, catchClause,
             * finallyBlock in that order. The oracle (harness/src/oracle-ast.ts)
             * has no explicit case for TryStatement, so it falls through to
             * the default branch which serialises forEachChild's visits as a
             * single `children` array (only when non-empty). Undefined
             * children are skipped the way ts.forEachChild / visitNode does;
             * for the 106_parserMissingToken1.ts fixture (`a / finally` at
             * EOF) that yields `children:[<tryBlock>, <finallyBlock>]` since
             * catchClause is absent. */
            size_t child_count = 0;
            if (n->data.tryStatement.tryBlock) child_count++;
            if (n->data.tryStatement.catchClause) child_count++;
            if (n->data.tryStatement.finallyBlock) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.tryStatement.tryBlock) {
                    emit_node(j, n->data.tryStatement.tryBlock);
                }
                if (n->data.tryStatement.catchClause) {
                    emit_node(j, n->data.tryStatement.catchClause);
                }
                if (n->data.tryStatement.finallyBlock) {
                    emit_node(j, n->data.tryStatement.finallyBlock);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_CatchClause: {
            /* forEachChildInCatchClause (~892): variableDeclaration, block. */
            size_t child_count = 0;
            if (n->data.catchClause.variableDeclaration) child_count++;
            if (n->data.catchClause.block) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.catchClause.variableDeclaration) {
                    emit_node(j, n->data.catchClause.variableDeclaration);
                }
                if (n->data.catchClause.block) {
                    emit_node(j, n->data.catchClause.block);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_ArrowFunction: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInFunctionLikeDeclaration: visits modifiers,
             * asteriskToken, name, questionToken, exclamationToken,
             * typeParameters, parameters, type, equalsGreaterThanToken, body
             * (in that order). The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for ArrowFunction, so it falls through to the
             * default branch which serialises forEachChild's visits as a
             * single `children` array (only when non-empty). ctsc currently
             * models `typeParameters` (when present), `parameters`,
             * `equalsGreaterThanToken`, and `body`; modifiers / return-type
             * annotation are still omitted from JSON until needed.
             *
             * equalsGreaterThanToken is a token leaf emitted as
             * {kind,pos,end}, mirroring tsc's parseExpectedToken +
             * finishNode path (parser.ts parseParenthesizedArrowFunctionExpression
             * ~5498 / parseSimpleArrowFunctionExpression ~5210).
             */
            size_t child_count = n->data.arrowFunction.type_parameters.len
                + n->data.arrowFunction.parameters.len + 1 /* => */
                + (n->data.arrowFunction.body ? 1 : 0);
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                for (size_t i = 0; i < n->data.arrowFunction.type_parameters.len; ++i) {
                    emit_node(j, n->data.arrowFunction.type_parameters.items[i]);
                }
                for (size_t i = 0; i < n->data.arrowFunction.parameters.len; ++i) {
                    emit_node(j, n->data.arrowFunction.parameters.items[i]);
                }
                ctsc_json_begin_obj(j);
                ctsc_json_key(j, "kind");
                ctsc_json_cstr(j, ctsc_syntax_kind_name(CTSC_SK_EqualsGreaterThanToken));
                ctsc_json_key(j, "pos");
                ctsc_json_int(j, n->data.arrowFunction.equals_greater_than_pos);
                ctsc_json_key(j, "end");
                ctsc_json_int(j, n->data.arrowFunction.equals_greater_than_end);
                ctsc_json_end_obj(j);
                if (n->data.arrowFunction.body) {
                    emit_node(j, n->data.arrowFunction.body);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
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
        case CTSC_SK_ObjectBindingPattern:
        case CTSC_SK_ArrayBindingPattern: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInBindingPattern (~676): visits each element in
             * `elements`. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for ObjectBindingPattern / ArrayBindingPattern,
             * so the default branch serializes forEachChild visits as a
             * `children` array (omitting the key when empty). An empty
             * `{ }` pattern therefore emits only `{kind,pos,end}`, matching
             * the 107_parserErrorRecovery_ParameterList5.ts fixture. */
            if (n->data.bindingPattern.elements.len > 0) {
                emit_array(j, "children", &n->data.bindingPattern.elements);
            }
            break;
        }
        case CTSC_SK_BindingElement: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInBindingElement (~681): visits dotDotDotToken,
             * propertyName, name, initializer. Default-branch serialization. */
            size_t child_count = (n->data.bindingElement.has_dotdotdot ? 1 : 0)
                + (n->data.bindingElement.propertyName ? 1 : 0)
                + (n->data.bindingElement.name ? 1 : 0)
                + (n->data.bindingElement.initializer ? 1 : 0);
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.bindingElement.has_dotdotdot) {
                    ctsc_json_begin_obj(j);
                    ctsc_json_key(j, "kind"); ctsc_json_cstr(j, ctsc_syntax_kind_name(CTSC_SK_DotDotDotToken));
                    ctsc_json_key(j, "pos");  ctsc_json_int(j, n->data.bindingElement.dotdotdot_pos);
                    ctsc_json_key(j, "end");  ctsc_json_int(j, n->data.bindingElement.dotdotdot_end);
                    ctsc_json_end_obj(j);
                }
                if (n->data.bindingElement.propertyName) {
                    emit_node(j, n->data.bindingElement.propertyName);
                }
                if (n->data.bindingElement.name) {
                    emit_node(j, n->data.bindingElement.name);
                }
                if (n->data.bindingElement.initializer) {
                    emit_node(j, n->data.bindingElement.initializer);
                }
                ctsc_json_end_arr(j);
            }
            break;
        }
        case CTSC_SK_TemplateExpression: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInTemplateExpression (~794): visits `head` then
             * each element of `templateSpans`. The oracle
             * (harness/src/oracle-ast.ts) has no explicit case, so it falls
             * through to the default branch which serialises the visits as
             * a single `children` array. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.templateExpression.head);
            for (size_t i = 0; i < n->data.templateExpression.templateSpans.len; ++i) {
                emit_node(j, n->data.templateExpression.templateSpans.items[i]);
            }
            ctsc_json_end_arr(j);
            break;
        }
        case CTSC_SK_TemplateSpan: {
            /* Mirrors forEachChildInTemplateSpan (~924): visits expression
             * then literal. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.templateSpan.expression);
            emit_node(j, n->data.templateSpan.literal);
            ctsc_json_end_arr(j);
            break;
        }
        case CTSC_SK_TaggedTemplateExpression: {
            /* Mirrors forEachChildInTaggedTemplateExpression (~748): tag then template. */
            ctsc_json_key(j, "children");
            ctsc_json_begin_arr(j);
            emit_node(j, n->data.taggedTemplateExpression.tag);
            emit_node(j, n->data.taggedTemplateExpression.template_);
            ctsc_json_end_arr(j);
            break;
        }
        case CTSC_SK_ModuleBlock: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInModuleBlock (~988): visits each element of
             * `statements`. The oracle (harness/src/oracle-ast.ts) has no
             * explicit case for ModuleBlock, so it falls through to the
             * default branch which serialises forEachChild's visits as a
             * single `children` array (only when non-empty). An empty
             * `namespace foo { }` body therefore emits `{kind,pos,end}`
             * alone, matching the 108_parserModuleDeclaration6.ts oracle
             * byte-for-byte (`{"kind":"ModuleBlock","pos":36,"end":41}`). */
            if (n->data.moduleBlock.statements.len > 0) {
                emit_array(j, "children", &n->data.moduleBlock.statements);
            }
            break;
        }
        case CTSC_SK_ModuleDeclaration: {
            /* Mirrors upstream/TypeScript/src/compiler/parser.ts
             * forEachChildInModuleDeclaration (~984): visits modifiers, name,
             * body (in that order). The oracle (harness/src/oracle-ast.ts)
             * has no explicit case for ModuleDeclaration, so it falls
             * through to the default branch which serialises forEachChild's
             * visits as a single `children` array (only when non-empty).
             * ctsc currently models only `name` and `body`; modifiers are
             * skipped until a fixture demands them. `name` is always
             * present (zero-width missing Identifier when the source
             * follows `namespace` / `module` with a reserved word). `body`
             * is omitted for the `declare module "foo";` shape (not yet
             * unlocked), which would emit `children:[name]` alone. */
            size_t child_count = 0;
            if (n->data.moduleDeclaration.name) child_count++;
            if (n->data.moduleDeclaration.body) child_count++;
            if (child_count > 0) {
                ctsc_json_key(j, "children");
                ctsc_json_begin_arr(j);
                if (n->data.moduleDeclaration.name) {
                    emit_node(j, n->data.moduleDeclaration.name);
                }
                if (n->data.moduleDeclaration.body) {
                    emit_node(j, n->data.moduleDeclaration.body);
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
