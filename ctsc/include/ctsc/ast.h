#ifndef CTSC_AST_H
#define CTSC_AST_H

#include "common.h"
#include "scanner.h"

/*
 * AST 节点定义。
 *
 * 形状约束：与 oracle-ast.ts 产出的 JSON 节点字段名、嵌套顺序完全一致；
 * 比较以 JSON 递归结构对齐方式进行（参见 harness/src/differ.ts diffAst）。
 * 因此不强制按 tsc 的字段枚举顺序写 JSON，但字段名必须匹配。
 *
 * 节点 kind 复用 CtscSyntaxKind（scanner.h）；新增时同步 token_names.c。
 */

typedef struct CtscNode CtscNode;

typedef struct {
    CtscNode** items;
    size_t     len;
    size_t     cap;
} CtscNodeArray;

typedef struct {
    CtscNodeArray statements;
    /*
     * End position of the statements NodeArray (UTF-16 offset). Mirrors
     * upstream/TypeScript/src/compiler/parser.ts parseList which sets
     * `nodeArray.end = scanner.getTokenFullStart()` after the last element;
     * for an empty file this equals the EOF token's full_start (= 0 when the
     * whole source is leading trivia, else the start of the first skipped
     * token). The emitter uses this to replay leading comments at the
     * statements boundary (see emitter.ts emitSourceFile →
     * emitBodyWithDetachedComments + the trailing emitLeadingComments call).
     */
    int           statements_end;
} CtscSourceFileData;

typedef struct {
    CtscNodeArray statements;
} CtscBlockData;

typedef struct {
    const uint16_t* text;
    size_t          text_len;
} CtscIdentifierData;

/*
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts scanNumber (~1233):
 *   - `text` is the canonical tokenValue that tsc stores on the node
 *     (LegacyOctal/ContainsLeadingZero are coerced via `"" + +result`;
 *     decimal/scientific are likewise coerced; plain integers keep the
 *     source substring).
 *   - `source_text` / `source_text_len` cover the on-disk lexeme that the
 *     emitter uses when `canUseOriginalText` (upstream utilities.ts ~2036)
 *     returns true. That utility disables source-text reuse when the
 *     literal carries TokenFlags.IsInvalid (Octal or ContainsLeadingZero);
 *     we encode that by setting `source_text` to NULL for those tokens.
 */
typedef struct {
    const uint16_t* text;
    size_t          text_len;
    const uint16_t* source_text;
    size_t          source_text_len;
} CtscNumericLiteralData;

typedef struct {
    const uint16_t* text;
    size_t          text_len;
    const uint16_t* value;
    size_t          value_len;
    bool            single_quote;
} CtscStringLiteralData;

typedef struct {
    const uint16_t* text;
    size_t          text_len;
} CtscRegularExpressionLiteralData;

/*
 * Mirrors upstream/TypeScript/src/compiler/types.ts TemplateLiteralLikeNode:
 * NoSubstitutionTemplateLiteral / TemplateHead / TemplateMiddle / TemplateTail
 * all carry `text` (decoded value) and reuse `getLiteralTextOfNode` for
 * emission. For `canUseOriginalText` (utilities.ts ~2036) returning true the
 * emitter writes the on-disk lexeme verbatim (including the surrounding
 * backticks / `${` / `}` delimiters), which is what `text`/`text_len` store
 * here — the scanner populates them with `source + token_start`.
 *
 * See emitter.ts emitLiteral (~2118) → getLiteralText (utilities.ts ~1980):
 * terminated template literals with a parent fall into the original-source
 * branch, so ctsc just copies the bytes back out.
 */
typedef struct {
    const uint16_t* text;
    size_t          text_len;
} CtscTemplateLiteralLikeData;

typedef struct {
    CtscNode* expression;
} CtscExpressionStatementData;

typedef struct {
    CtscNode* expression;  /* nullable */
} CtscReturnStatementData;

typedef struct {
    /* NodeFlags mirror: bit0 = Let, bit1 = Const (else Var). */
    int           flags;
    CtscNodeArray declarations;
} CtscVariableDeclarationListData;

typedef struct {
    CtscNode* declarationList;
} CtscVariableStatementData;

typedef struct {
    CtscNode* name;        /* Identifier */
    CtscNode* type;        /* nullable, TypeNode */
    CtscNode* initializer; /* nullable */
} CtscVariableDeclarationData;

typedef struct {
    CtscNode*       left;
    CtscSyntaxKind  operator_kind;
    CtscNode*       right;
} CtscBinaryExpressionData;

typedef struct {
    CtscNode*     expression;
    CtscNodeArray arguments;
} CtscCallExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseNewExpressionOrNewDotTarget
 * (~6801): `new MemberExpression (TypeArguments)? (Arguments)?`. The optional
 * `arguments` NodeArray is absent when the source omits `()` (e.g. `new Date`),
 * matching tsc's NewExpression.arguments being `undefined` in that case.
 *
 * The oracle (harness/src/oracle-ast.ts) has no explicit case for NewExpression,
 * so it falls through to the default branch which serializes
 * forEachChildInCallOrNewExpression (~1158) visits — expression, typeArguments,
 * arguments — as a single `children` array. ast_json.c mirrors that by emitting
 * `children:[expression, ...arguments]` (ctsc does not model typeArguments yet;
 * tsc rolls the try-parse back when `<...` fails to close, as in the
 * 105_parserConstructorAmbiguity2.ts fixture).
 */
typedef struct {
    CtscNode*     expression;
    bool          has_arguments;
    CtscNodeArray arguments;
} CtscNewExpressionData;

typedef struct {
    CtscNode*     name;       /* Identifier nullable (anon) */
    CtscNodeArray parameters;
    CtscNode*     body;       /* Block */
} CtscFunctionDeclarationData;

typedef struct {
    CtscNode* name;        /* Identifier */
    CtscNode* type;        /* nullable */
    CtscNode* initializer; /* nullable */
} CtscParameterData;

typedef struct {
    CtscNode* expression;
    CtscNode* thenStatement;
    CtscNode* elseStatement; /* nullable */
} CtscIfStatementData;

typedef struct {
    CtscNode* expression;
    CtscNode* statement;
} CtscWhileStatementData;

typedef struct {
    CtscNode* initializer; /* nullable: VariableDeclarationList or expression */
    CtscNode* condition;   /* nullable */
    CtscNode* incrementor; /* nullable */
    CtscNode* statement;
} CtscForStatementData;

typedef struct {
    CtscNode* expression;
} CtscParenthesizedExpressionData;

typedef struct {
    CtscSyntaxKind operator_kind;
    CtscNode*      operand;
} CtscPrefixUnaryExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseVoidExpression (~5708)
 * and forEachChildInVoidExpression (~767, which visits `node.expression`). The
 * oracle (harness/src/oracle-ast.ts) serializes this kind via the default
 * branch, which emits forEachChild's visits as a `children` array. We mirror
 * that by emitting `children:[expression]` from ast_json.c. TypeOfExpression
 * and DeleteExpression have the same shape and will reuse this struct when
 * those fixtures unlock.
 */
typedef struct {
    CtscNode* expression;
} CtscVoidExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseYieldExpression (~5169):
 *     YieldExpression[In] :
 *       yield
 *       yield [no LineTerminator here] [Lexical goal InputElementRegExp]AssignmentExpression[?In, Yield]
 *       yield [no LineTerminator here] * [Lexical goal InputElementRegExp]AssignmentExpression[?In, Yield]
 * Both `asteriskToken` and `expression` are optional (the bare `yield` form
 * sets them to undefined). The oracle (harness/src/oracle-ast.ts) has no
 * explicit case for YieldExpression, so it falls through to the default
 * branch which serializes forEachChildInYieldExpression (~773, visits
 * asteriskToken then expression) as a `children` array. ast_json.c mirrors
 * that, skipping the undefined children the way ts.forEachChild does.
 */
typedef struct {
    bool       has_asterisk;
    int        asterisk_pos;
    int        asterisk_end;
    CtscNode*  expression; /* nullable */
} CtscYieldExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseUpdateExpression (~5875):
 * LeftHandSideExpression [[no LineTerminator here]] ++ / --.
 * The oracle (harness/src/oracle-ast.ts) serializes this kind via the default
 * branch, which walks forEachChildInPostfixUnaryExpression (visits only the
 * operand) and emits `children:[operand]`. See ast_json.c for the matching
 * emission.
 */
typedef struct {
    CtscSyntaxKind operator_kind;
    CtscNode*      operand;
} CtscPostfixUnaryExpressionData;

typedef struct {
    CtscNode* expression;
    CtscNode* name; /* Identifier */
} CtscPropertyAccessExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseElementAccessExpressionRest
 * (~6432): `expression [ argumentExpression ]`. When the bracket is immediately
 * followed by `]`, tsc synthesises a zero-width missing Identifier as the
 * argument via createMissingNode(SyntaxKind.Identifier, ..., "An_element_access_
 * expression_should_take_an_argument") (parser.ts ~6435). The oracle
 * (harness/src/oracle-ast.ts) has no explicit case for ElementAccessExpression,
 * so it falls through to its default branch which serialises
 * forEachChildInElementAccessExpression (types.ts / parser.ts visitor ~1162,
 * visits expression, questionDotToken, argumentExpression) as a single
 * `children` array. ast_json.c mirrors that by emitting
 * `children:[expression, argumentExpression]` (ctsc does not model
 * questionDotToken on ElementAccess yet).
 */
typedef struct {
    CtscNode* expression;
    CtscNode* argumentExpression;
} CtscElementAccessExpressionData;

typedef struct {
    CtscNode* condition;
    CtscNode* whenTrue;
    CtscNode* whenFalse;
} CtscConditionalExpressionData;

typedef struct {
    CtscNodeArray properties;
} CtscObjectLiteralExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseArrayLiteralExpression
 * (~6689): "[" ArgumentOrArrayLiteralElement (, ...)* "]". The oracle
 * (harness/src/oracle-ast.ts) serializes this kind via the default branch,
 * which emits forEachChild's visits as a `children` array. We mirror that
 * by emitting `children:[elements...]` from ast_json.c.
 */
typedef struct {
    CtscNodeArray elements;
} CtscArrayLiteralExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseBreakOrContinueStatement (~6977):
 * both BreakStatement and ContinueStatement carry an optional `label` Identifier.
 */
typedef struct {
    CtscNode* label; /* nullable Identifier */
} CtscBreakOrContinueStatementData;

struct CtscNode {
    CtscSyntaxKind kind;
    int            pos;
    int            end;
    union {
        CtscSourceFileData              sourceFile;
        CtscBlockData                   block;
        CtscIdentifierData              identifier;
        CtscNumericLiteralData          numericLiteral;
        CtscStringLiteralData           stringLiteral;
        CtscRegularExpressionLiteralData regularExpressionLiteral;
        CtscTemplateLiteralLikeData     templateLiteralLike;
        CtscExpressionStatementData     expressionStatement;
        CtscReturnStatementData         returnStatement;
        CtscVariableStatementData       variableStatement;
        CtscVariableDeclarationListData variableDeclarationList;
        CtscVariableDeclarationData     variableDeclaration;
        CtscBinaryExpressionData        binaryExpression;
        CtscCallExpressionData          callExpression;
        CtscNewExpressionData           newExpression;
        CtscFunctionDeclarationData     functionDeclaration;
        CtscParameterData               parameter;
        CtscIfStatementData             ifStatement;
        CtscWhileStatementData          whileStatement;
        CtscForStatementData            forStatement;
        CtscParenthesizedExpressionData parenthesizedExpression;
        CtscPrefixUnaryExpressionData   prefixUnaryExpression;
        CtscPostfixUnaryExpressionData  postfixUnaryExpression;
        CtscPropertyAccessExpressionData propertyAccessExpression;
        CtscElementAccessExpressionData elementAccessExpression;
        CtscConditionalExpressionData   conditionalExpression;
        CtscObjectLiteralExpressionData objectLiteralExpression;
        CtscArrayLiteralExpressionData  arrayLiteralExpression;
        CtscBreakOrContinueStatementData breakOrContinueStatement;
        CtscVoidExpressionData          voidExpression;
        CtscYieldExpressionData         yieldExpression;
    } data;
};

struct CtscArena;
CtscNode* ctsc_node_new(struct CtscArena* a, CtscSyntaxKind kind, int pos, int end);
void      ctsc_node_array_init(CtscNodeArray* arr);
void      ctsc_node_array_push(CtscNodeArray* arr, struct CtscArena* a, CtscNode* n);

#endif
