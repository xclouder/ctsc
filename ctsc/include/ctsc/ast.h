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
    /*
     * Full source as UTF-16 code units (arena copy of the parser scanner
     * buffer). Used by the checker for postfix `T[]` detection: under noLib,
     * getTypeAtLocation stringifies intrinsic `T[]` as `{}` while function
     * signatures still print `T[]` (checker.ts / typeToString paths).
     */
    const uint16_t* text_utf16;
    size_t          text_utf16_len;
} CtscSourceFileData;

typedef struct {
    CtscNodeArray statements;
    /*
     * Mirrors upstream/TypeScript/src/compiler/parser.ts parseBlock (~6824):
     * `multiLine = scanner.hasPrecedingLineBreak()` captured after the
     * opening brace is consumed — i.e. true when a line break separates
     * `{` from the first token of the block body. The printer uses this
     * (emitter.ts emitBlock ~3036) to choose SingleLineBlockStatements
     * vs MultiLineBlockStatements: an empty Block with multiLine=true
     * renders as `{\n}` via emitNodeList's MultiLine isEmpty branch
     * (~4703), otherwise it collapses to `{ }` via SpaceBetweenBraces.
     */
    bool          multi_line;
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
 *
 * `value` / `value_len` mirror the scanner's decoded template fragment
 * (checker.ts getFreshTypeOfTemplateLiteral / LiteralExpression text for
 * NoSubstitutionTemplateLiteral — the cooked string without delimiters).
 */
typedef struct {
    const uint16_t* text;
    size_t          text_len;
    const uint16_t* value;
    size_t          value_len;
} CtscTemplateLiteralLikeData;

/*
 * Mirrors upstream/TypeScript/src/compiler/types.ts TemplateExpression
 * (`head: TemplateHead; templateSpans: NodeArray<TemplateSpan>`) and
 * TemplateSpan (`expression: Expression; literal: TemplateMiddle |
 * TemplateTail`). Parsed by parser.ts parseTemplateExpression (~3668) /
 * parseTemplateSpan (~3724), emitted by emitter.ts emitTemplateExpression
 * (~2966) / emitTemplateSpan (~3027): head then each span's
 * expression followed by its literal.
 */
typedef struct {
    CtscNode*     head;          /* TemplateHead literal-like */
    CtscNodeArray templateSpans; /* CtscTemplateSpan nodes */
} CtscTemplateExpressionData;

typedef struct {
    CtscNode* expression; /* any Expression */
    CtscNode* literal;    /* TemplateMiddle | TemplateTail */
} CtscTemplateSpanData;

/*
 * Mirrors upstream/TypeScript/src/compiler/types.ts TaggedTemplateExpression
 * (`tag`, `template`: NoSubstitutionTemplateLiteral | TemplateExpression).
 * Parsed by parser.ts parseTaggedTemplateRest (~6505), emitted by emitter.ts
 * emitTaggedTemplateExpression (~2722).
 */
typedef struct {
    CtscNode* tag;
    CtscNode* template_; /* NoSubstitutionTemplateLiteral | TemplateExpression */
} CtscTaggedTemplateExpressionData;

typedef struct {
    CtscNode* expression;
} CtscExpressionStatementData;

typedef struct {
    CtscNode* expression;  /* nullable */
} CtscReturnStatementData;

typedef struct {
    CtscNode* expression;
} CtscThrowStatementData;

typedef struct {
    /* NodeFlags mirror: bit0 = Let, bit1 = Const (else Var). */
    int           flags;
    CtscNodeArray declarations;
} CtscVariableDeclarationListData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseVariableStatement
 * (~7727): `VariableStatement : (modifiers)? VarDeclaration`. ctsc models the
 * modifiers list sparsely — only `has_declare` is tracked, mirroring the
 * `DeclareKeyword` modifier that marks an ambient declaration
 * (parser.ts isDeclareModifier ~7463). The TS-to-JS transformer elides every
 * statement with `ModifierFlags.Ambient` via createNotEmittedStatement
 * (transformers/ts.ts visitTypeScript ~643), so the emitter treats
 * `has_declare` VariableStatements as dropped (emitter.c
 * source_file_statement_is_dropped). When a fixture demands richer modifier
 * tracking (`export`, etc.) extend this struct alongside the ambient-elision
 * rule. The parser-AST oracle (harness/src/oracle-ast.ts) emits only
 * `declarationList` for VariableStatement and omits the modifiers NodeArray,
 * so `has_declare` is not surfaced in ast_json.c either.
 */
typedef struct {
    CtscNode* declarationList;
    bool      has_declare;
    /* SourceFile-level `export const` / `export var` / `export let` (parseStatement
     * ExportKeyword branch). Only VariableStatement uses this; elided otherwise. */
    bool      has_export;
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

/*
 * Mirrors upstream CallExpression: optional type arguments from the
 * `Callee<TypeArgs>(Args)` form (parser.ts parseCallExpressionRest ~6520 +
 * parseTypeArgumentsInExpression). ctsc absorbs `<...>` like NewExpression;
 * ast_json keeps emitting only expression/arguments for oracle parity.
 */
typedef struct {
    CtscNode*     expression;
    bool          has_type_arguments;
    CtscNodeArray type_arguments;
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
 * `children:[expression, ...typeArguments, ...arguments]`.
 *
 * `has_type_arguments` tracks whether a TypeArguments NodeArray was absorbed
 * from a speculative `<...>` parse (mirrors parser.ts parseMemberExpressionRest
 * wrapping `expr<T>` as an ExpressionWithTypeArguments, which
 * parseNewExpressionOrNewDotTarget then unpacks into the NewExpression). When
 * false, typeArguments is omitted from forEachChild traversal, matching the
 * `105_parserConstructorAmbiguity2.ts` rollback case where the try-parse fails.
 */
typedef struct {
    CtscNode*     expression;
    bool          has_type_arguments;
    CtscNodeArray type_arguments;
    bool          has_arguments;
    CtscNodeArray arguments;
} CtscNewExpressionData;

/*
 * FunctionDeclaration and FunctionExpression share this struct; only
 * CtscNode.kind distinguishes them. Mirrors upstream/TypeScript/src/compiler/
 * parser.ts forEachChildInFunctionLikeDeclaration (~548), which visits (in
 * order) modifiers, asteriskToken, name, questionToken, exclamationToken,
 * typeParameters, parameters, type, body.
 *
 * `has_asterisk` tracks whether the generator `*` was present after the
 * `function` keyword (parseFunctionExpression ~6765 / parseFunctionDeclaration
 * ~7734 both do `parseOptionalToken(AsteriskToken)`). The oracle
 * (harness/src/oracle-ast.ts) has an explicit FunctionDeclaration case that
 * emits name/parameters/body only, so FunctionDeclaration hides the asterisk;
 * FunctionExpression has no explicit case and falls through to the default
 * `children` branch, where the AsteriskToken leaf must appear between
 * name(nullable) and parameters per the forEachChild visit order. The pos/end
 * pair mirror parseTokenNode (~2553): pos = getNodePos() before consuming,
 * end = scanner.getTokenFullStart() after nextToken(). See fixture
 * `parser/from-upstream/108_FunctionExpression1_es6.ts` ("function * () {}").
 */
typedef struct {
    bool          has_asterisk;
    int           asterisk_pos;
    int           asterisk_end;
    /* True when `async` preceded `function` (parser.ts parseFunctionDeclaration). */
    bool          has_async;
    /* True for SourceFile-level `export function ...` (parser.ts parseDeclaration
     * ~7467 + parseFunctionDeclaration with ExportKeyword modifier). Only
     * FunctionDeclaration uses this; FunctionExpression leaves it false. */
    bool          has_export;
    /* True for `export default function ...` (parser.ts parseDeclaration ~7530-7535
     * → parseExportAssignment ~8736, expression parsed as function). Mutually
     * exclusive with has_export in normal sources. */
    bool          has_export_default;
    /* True for `declare function ...` (ModifierFlags.Ambient); elided in JS emit. */
    bool          has_declare;
    CtscNode*     name;       /* Identifier nullable (anon) */
    /* Mirrors parseTypeParameters (~3987) between name and parseParameters. */
    CtscNodeArray type_parameters;
    CtscNodeArray parameters;
    CtscNode*     body;       /* Block */
} CtscFunctionDeclarationData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseParenthesizedArrowFunctionExpression
 * (~5430) + parseSimpleArrowFunctionExpression (~5197):
 *     ArrowFunction:
 *         (modifiers)? TypeParameters? ( ParameterList? ) ReturnType? `=>` ConciseBody
 *         BindingIdentifier `=>` ConciseBody
 * ConciseBody is either a Block (when the source has `{ ... }`) or an
 * AssignmentExpression (for the concise-body form, e.g. `x => x + 1`).
 *
 * forEachChildInFunctionLikeDeclaration visits modifiers, asteriskToken,
 * name, questionToken, exclamationToken, typeParameters, parameters, type,
 * equalsGreaterThanToken, body. The oracle (harness/src/oracle-ast.ts) has
 * no explicit case for ArrowFunction so it falls through to the default
 * `children` branch. ctsc currently models only the subset exercised by
 * 107_parserArrowFunctionExpression2.ts (`a = () => { } || a`): a zero-
 * parameter arrow with a Block body and no return type annotation. The
 * EqualsGreaterThanToken pos/end is captured so ast_json.c can emit it as
 * a synthetic token child between `parameters` and `body`.
 */
typedef struct {
    bool          has_async;
    /* Mirrors parseParenthesizedArrowFunctionExpression (~5430): optional
     * type parameters before `(`. Elided in JS emit; populated for generic
     * arrows like `<T>(a) => ...`. */
    CtscNodeArray type_parameters;
    CtscNodeArray parameters;
    int           equals_greater_than_pos;
    int           equals_greater_than_end;
    CtscNode*     body; /* Block or AssignmentExpression */
} CtscArrowFunctionData;

/* Mirrors upstream/TypeScript/src/compiler/parser.ts parseAwaitExpression (~5726). */
typedef struct {
    CtscNode* expression;
} CtscAwaitExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseParameterWorker
 * (~4036) + forEachChildInParameter (~528): a ParameterDeclaration carries
 * `modifiers`, `dotDotDotToken`, `name`, `questionToken`, `type`, and
 * `initializer`. ctsc models `name`, `type`, `initializer`, plus rest-param
 * and `is_optional` (the `?` after the name is omitted in JS emit per
 * transformers/ts.ts visitParameter).
 *
 * `has_dot_dot_dot` is true for rest parameters. The token's span is kept
 * purely so future AST-JSON / forEachChild work can produce a synthetic
 * DotDotDotToken leaf at the right position without another parser pass.
 *
 * `is_optional` is true after parseOptionalToken(QuestionToken) following the
 * binding name (parser.ts parseParameterWorker ~4081). Used by the checker for
 * signature type strings and arity (mirrors checker.ts optional parameters).
 */
typedef struct {
    CtscNode* name;        /* Identifier | ObjectBindingPattern | ArrayBindingPattern */
    CtscNode* type;        /* nullable */
    CtscNode* initializer; /* nullable */
    bool      has_dot_dot_dot;
    int       dot_dot_dot_pos;
    int       dot_dot_dot_end;
    /* True when parseParameter consumed a public/private/protected/readonly
     * modifier (mirrors isParameterPropertyDeclaration in utilitiesPublic.ts).
     * Used by the emitter to inject this.<name> = <name> in constructors. */
    bool      is_parameter_property;
    bool      is_optional;
} CtscParameterData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectBindingPattern
 * (~4860) / parseArrayBindingPattern (~4877): a BindingPattern node whose
 * `elements` NodeArray holds BindingElement (and OmittedExpression for array
 * `[, x]` holes) nodes. The oracle (harness/src/oracle-ast.ts) has no explicit
 * case for ObjectBindingPattern / ArrayBindingPattern / BindingElement, so
 * they fall through to the default branch which serialises forEachChild's
 * visits as a `children` array. For an empty `{ }` pattern there are no
 * children and only `{kind,pos,end}` is emitted — which is what the
 * 107_parserErrorRecovery_ParameterList5.ts fixture relies on for the
 * error-recovery Parameter.name = ObjectBindingPattern shape.
 *
 * ctsc does not yet expand BindingElement beyond the empty-pattern case;
 * when a fixture demands `{ a }`, `{ a: b }`, `[a, ...rest]`, etc., grow
 * the parser and this struct to carry the element fields forEachChild
 * visits (propertyName, dotDotDotToken, name, initializer).
 */
typedef struct {
    CtscNodeArray elements; /* BindingElement / OmittedExpression list */
} CtscBindingPatternData;

typedef struct {
    CtscNode* propertyName;     /* nullable (object only) */
    CtscNode* name;             /* Identifier | BindingPattern */
    CtscNode* initializer;      /* nullable */
    bool      has_dotdotdot;
    int       dotdotdot_pos;
    int       dotdotdot_end;
} CtscBindingElementData;

typedef struct {
    CtscNode* expression;
    CtscNode* thenStatement;
    CtscNode* elseStatement; /* nullable */
} CtscIfStatementData;

typedef struct {
    CtscNode* expression;
    CtscNode* statement;
} CtscWhileStatementData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseDoStatement (~6897):
 *     DoStatement: `do` Statement `while` `(` Expression `)` `;`?
 * The trailing semicolon is parseOptional, mirroring ES spec's automatic
 * semicolon insertion carve-out for `do`/`while`. The oracle
 * (harness/src/oracle-ast.ts) has no explicit case for DoStatement so it
 * falls through to the default branch which serialises forEachChild visits
 * (statement, expression) as a single `children` array. ast_json.c mirrors
 * that.
 */
typedef struct {
    CtscNode* statement;
    CtscNode* expression;
} CtscDoStatementData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseWithStatement (~7000):
 *     WithStatement: `with` `(` Expression `)` Statement
 * forEachChildInWithStatement (parser.ts ~862) visits expression then
 * statement. The oracle (harness/src/oracle-ast.ts) has no explicit case for
 * WithStatement so it falls through to the default branch which serialises
 * those visits as a single `children` array. ast_json.c mirrors that.
 */
typedef struct {
    CtscNode* expression;
    CtscNode* statement;
} CtscWithStatementData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseSwitchStatement
 * (~7042) / factory.createSwitchStatement: `switch` `(` Expression `)` CaseBlock.
 * forEachChildInSwitchStatement visits expression then caseBlock.
 */
typedef struct {
    CtscNode* expression;
    CtscNode* caseBlock; /* CaseBlock */
} CtscSwitchStatementData;

/*
 * Mirrors parser.ts parseCaseBlock (~7034): `{` CaseOrDefaultClause* `}`.
 * forEachChildInCaseBlock visits clauses.
 */
typedef struct {
    CtscNodeArray clauses;
    bool          multi_line; /* `{` then line break before first clause (emit.ts) */
} CtscCaseBlockData;

/*
 * Mirrors parser.ts parseCaseClause (~7012): `case` Expression `:` Statement*.
 * forEachChildInCaseClause visits expression then each statement.
 */
typedef struct {
    CtscNode*     expression;
    CtscNodeArray statements;
} CtscCaseClauseData;

/*
 * Mirrors parser.ts parseDefaultClause (~7022): `default` `:` Statement*.
 */
typedef struct {
    CtscNodeArray statements;
} CtscDefaultClauseData;

typedef struct {
    CtscNode* initializer; /* nullable: VariableDeclarationList or expression */
    CtscNode* condition;   /* nullable */
    CtscNode* incrementor; /* nullable */
    CtscNode* statement;
} CtscForStatementData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseForOrForInOrForOfStatement
 * (~6928) and the factory createForInStatement / createForOfStatement. Shared
 * between ForInStatement and ForOfStatement (distinguished by CtscNode.kind).
 * forEachChildInForInStatement  (parser.ts ~846) visits initializer, expression, statement.
 * forEachChildInForOfStatement  (parser.ts ~851) visits awaitModifier, initializer,
 *                                                 expression, statement.
 * The oracle (harness/src/oracle-ast.ts) has no explicit case for either kind
 * and falls through to the default branch, which serialises forEachChild's
 * visits as a single `children` array. ast_json.c mirrors that.
 *
 * `awaitModifier` is not yet modelled (no active fixture uses `for await (... of ...)`).
 * When added, include it as a token leaf in the children array ahead of
 * `initializer`, matching the forEachChild order. */
typedef struct {
    CtscNode* initializer; /* VariableDeclarationList or expression */
    CtscNode* expression;
    CtscNode* statement;
} CtscForInOrOfStatementData;

typedef struct {
    CtscNode* expression;
} CtscParenthesizedExpressionData;

typedef struct {
    CtscSyntaxKind operator_kind;
    CtscNode*      operand;
} CtscPrefixUnaryExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseVoidExpression (~5708),
 * parseDeleteExpression (~5698), parseTypeOfExpression (~5703) and the
 * corresponding forEachChild* helpers (~767, ~763, ~765), each of which visits
 * only `node.expression`. The three node shapes are identical at both the
 * parser and factory level (factory.createVoidExpression / createDeleteExpression
 * / createTypeOfExpression all just wrap a single expression), so ctsc stores
 * them in a single shared data struct accessed as `n->data.voidExpression`
 * regardless of the node kind. The oracle (harness/src/oracle-ast.ts) has no
 * explicit case for any of them, so they all fall through to the default
 * branch which emits forEachChild's visits as a `children` array. We mirror
 * that by emitting `children:[expression]` from ast_json.c for each kind.
 */
typedef struct {
    CtscNode* expression;
} CtscVoidExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeAssertion (~6371):
 *     TypeAssertion: `<` Type `>` UnaryExpression
 * forEachChildInTypeAssertionExpression (parser.ts ~758) visits `type` then
 * `expression`. The oracle (harness/src/oracle-ast.ts) has no explicit case
 * for TypeAssertionExpression so it falls through to the default branch which
 * serialises those visits as a single `children` array. finishNode positions:
 * pos = full_start of the `<`; end = `expression.end` (scanner.getTokenFullStart
 * after parseSimpleUnaryExpression consumes the operand).
 */
typedef struct {
    CtscNode* type;
    CtscNode* expression;
} CtscTypeAssertionExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts forEachChildInAsExpression (~788):
 * visits `expression` then `type`.
 */
typedef struct {
    CtscNode* expression;
    CtscNode* type;
} CtscAsExpressionData;

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
    /*
     * True when this access used `?.` (optional chaining). Mirrors
     * PropertyAccessExpression.questionDotToken in upstream types.ts.
     */
    bool      optional_chain;
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
    bool      optional_chain; /* true when source was `?.[` */
} CtscElementAccessExpressionData;

typedef struct {
    CtscNode* condition;
    CtscNode* whenTrue;
    CtscNode* whenFalse;
} CtscConditionalExpressionData;

typedef struct {
    CtscNodeArray properties;
    /* Mirrors parser.ts parseObjectLiteralExpression (~6759): multiLine =
     * scanner.hasPrecedingLineBreak() after `{`; controls PreferNewLine in
     * emitObjectLiteralExpression (~2627). */
    bool          multi_line;
    bool          has_trailing_comma;
} CtscObjectLiteralExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parsePropertyAssignment
 * branch of parseObjectLiteralElement (~6743): `PropertyName : AssignmentExpression`.
 * The oracle (harness/src/oracle-ast.ts) has no explicit case for
 * PropertyAssignment so it falls through to the default branch which
 * serialises forEachChildInPropertyAssignment's visits (modifiers, name,
 * questionToken, exclamationToken, initializer) as a single `children`
 * array. ctsc currently only models `name` + `initializer` (modifiers /
 * questionToken / exclamationToken are skipped until a fixture demands them).
 * When parseExpected(ColonToken) fails on an object-literal element like
 * `{ [e] }`, tsc's parseAssignmentExpressionOrHigher falls through to
 * parseIdentifier(Diagnostics.Expression_expected) which returns a zero-
 * width missing Identifier at scanner.getTokenFullStart() — mirrored here
 * by the parser emitting a missing Identifier for `initializer`.
 */
typedef struct {
    CtscNode* name;
    CtscNode* initializer;
} CtscPropertyAssignmentData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectLiteralElement
 * (~6733) ShorthandPropertyAssignment branch: `IdentifierReference Initializer?`
 * (the `Initializer` covers the CoverInitializedName grammar used to back
 * ObjectAssignmentPattern). forEachChildInShorthandPropertyAssignment visits
 * modifiers, name, questionToken, exclamationToken, equalsToken,
 * objectAssignmentInitializer. ctsc currently models `name` and the optional
 * `objectAssignmentInitializer` only; the oracle default branch emits them as
 * a `children` array.
 */
typedef struct {
    CtscNode* name;
    CtscNode* objectAssignmentInitializer; /* nullable */
} CtscShorthandPropertyAssignmentData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseObjectLiteralElement
 * (~6703): `...` AssignmentExpression. forEachChildInSpreadAssignment visits
 * only `expression` (types.ts SpreadAssignment).
 */
typedef struct {
    CtscNode* expression;
} CtscSpreadAssignmentData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseArgumentExpression
 * (~6685): SpreadElement — `...` UnaryExpression (types.ts SpreadElement).
 * forEachChild visits only `expression`, matching SpreadAssignment's shape.
 */
typedef struct {
    CtscNode* expression;
} CtscSpreadElementData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseComputedPropertyName
 * (~2733): `[ Expression ]`. forEachChildInComputedPropertyName visits only
 * `expression`, so the oracle default branch serialises `children:[expression]`.
 * finishNode positions: pos = full_start of `[`; end = scanner.getTokenFullStart()
 * of the token AFTER `]` (== cur_full_start(p) right after consuming `]`).
 */
typedef struct {
    CtscNode* expression;
} CtscComputedPropertyNameData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseArrayLiteralExpression
 * (~6689): "[" ArgumentOrArrayLiteralElement (, ...)* "]". The oracle
 * (harness/src/oracle-ast.ts) serializes this kind via the default branch,
 * which emits forEachChild's visits as a `children` array. We mirror that
 * by emitting `children:[elements...]` from ast_json.c.
 *
 * `has_trailing_comma` mirrors NodeArray.hasTrailingComma recorded by
 * parser.ts parseDelimitedList (~3458): when the separator `,` is consumed
 * and the next token is the list terminator, the resulting NodeArray is
 * flagged with hasTrailingComma=true. The emitter forwards that flag
 * through ListFormat.AllowTrailingComma (emitter.ts emitNodeListItems
 * ~4824) so `[1, 1,]` round-trips its trailing comma — see
 * parserArrayLiteralExpression10.ts.
 */
typedef struct {
    CtscNodeArray elements;
    bool          has_trailing_comma;
} CtscArrayLiteralExpressionData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseBreakOrContinueStatement (~6977):
 * both BreakStatement and ContinueStatement carry an optional `label` Identifier.
 */
typedef struct {
    CtscNode* label; /* nullable Identifier */
} CtscBreakOrContinueStatementData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseExpressionOrLabeledStatement
 * (~7123) + factory.createLabeledStatement: IdentifierReference `:` Statement.
 */
typedef struct {
    CtscNode* label;     /* Identifier */
    CtscNode* statement; /* nullable on hard parse failure */
} CtscLabeledStatementData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTryStatement (~7078):
 *     TryStatement:
 *       try Block Catch
 *       try Block Finally
 *       try Block Catch Finally
 * `tryBlock` is always present (parseBlock with ignoreMissingOpenBrace=false
 * returns a zero-width Block when `{` is missing, per parser.ts parseBlock
 * ~6841). `catchClause` is optional; when absent OR when the current token
 * is FinallyKeyword, the parser also parses `finally Block` and sets
 * `finallyBlock`. `finallyBlock` is nullable (the normal `try { } catch { }`
 * form omits it).
 *
 * The oracle (harness/src/oracle-ast.ts) has no explicit case for
 * TryStatement, so it falls through to the default branch which serialises
 * forEachChildInTryStatement visits (tryBlock, catchClause, finallyBlock) as
 * a single `children` array. ast_json.c mirrors that, skipping any undefined
 * children the way ts.forEachChild / visitNode does.
 *
 * `catchClause` is populated by parseCatchClause (~7097) when the token after
 * the try block is `catch`. The 106_parserMissingToken1.ts fixture (`a / finally`
 * at EOF) still has `catchClause` NULL.
 */
typedef struct {
    CtscNode* tryBlock;      /* Block, always present */
    CtscNode* catchClause;   /* nullable CatchClause */
    CtscNode* finallyBlock;  /* nullable Block */
} CtscTryStatementData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseCatchClause (~7097):
 *   catch ( VariableDeclaration? ) Block
 * `variableDeclaration` is absent when the `( )` pair is omitted (optional
 * binding in ES2019 optional catch binding).
 */
typedef struct {
    CtscNode* variableDeclaration; /* nullable VariableDeclaration */
    CtscNode* block;               /* Block */
} CtscCatchClauseData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseMethodDeclaration (~7782):
 *   MethodDeclaration (in an ObjectLiteral):
 *     (modifiers)? (`*`)? PropertyName (`?`)? (`!`)?
 *       TypeParameters? `(` Parameters? `)` ReturnType? Body
 * The oracle (harness/src/oracle-ast.ts) has no explicit case for
 * MethodDeclaration, so it falls through to the default branch which
 * serializes forEachChildInMethodDeclaration's visits as a single
 * `children` array. forEachChild for MethodDeclaration visits, in order:
 *   modifiers, asteriskToken, name, questionToken, exclamationToken,
 *   typeParameters, parameters, type, body.
 * ctsc models modifiers, asteriskToken, name, typeParameters, parameters,
 * optional return-type `type`, and body; questionToken / exclamationToken
 * are omitted until fixtures require them.
 *
 * `has_asterisk` tracks whether the `*` token was present so the JSON
 * emitter can include an AsteriskToken child leaf with the recorded pos/end
 * (parser.ts parseTokenNode ~2553 records pos = getNodePos and
 * end = scanner.getTokenFullStart() after consuming the token). `name` is
 * non-null (parsePropertyName falls back to createMissingNode(Identifier)
 * per parser.ts ~4210 when no property-name-starting token is present).
 * `body` is nullable to mirror parseFunctionBlockOrSemicolon, which returns
 * undefined when a semicolon stands in for the body (ambient / overload).
 *
 * `type_parameters` mirrors upstream parseTypeParameters (~3987): when the
 * method signature begins with `<` (e.g. `{ *<T>() { } }` from
 * 107_FunctionPropertyAssignments6_es6.ts), the bracketed list of
 * TypeParameter declarations is captured here. The emitter inserts these
 * children between `name` and `parameters` to match
 * forEachChildInMethodDeclaration (parser.ts ~530) visit order.
 */
typedef struct {
    CtscNodeArray modifiers;        /* ModifierLike token leaves; may be empty */
    bool          has_asterisk;
    int           asterisk_pos;
    int           asterisk_end;
    CtscNode*     name;           /* Identifier (possibly missing, zero-width) */
    CtscNodeArray type_parameters;
    CtscNodeArray parameters;
    /* Nullable `:` ReturnType from parseMethodDeclaration (parser.ts ~7796). */
    CtscNode*     type;
    CtscNode*     body;           /* nullable Block */
} CtscMethodDeclarationData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseAccessorDeclaration
 * (~7851):
 *   AccessorDeclaration (in an ObjectLiteral):
 *     (modifiers)? (`get`|`set`) PropertyName TypeParameters?
 *       `(` Parameters? `)` ReturnType? Body
 * The oracle (harness/src/oracle-ast.ts) has no explicit case for
 * GetAccessor / SetAccessor, so it falls through to the default branch
 * which serializes forEachChildInGetAccessor / forEachChildInSetAccessor
 * (parser.ts ~617 / ~625) visits as a single `children` array. For both
 * kinds forEachChild visits, in order:
 *   modifiers, name, typeParameters, parameters, type, body.
 *
 * ctsc currently models only `name`, `parameters`, and `body` — modifiers,
 * typeParameters, and the (setter-only) return-type annotation are skipped
 * until a fixture demands them (the initial unlock is
 * 108_parserComputedPropertyName4.ts: `var v = { get [e]() { } };` which
 * exercises the bare `get ComputedPropertyName () Block` shape). `name` is
 * non-null (parsePropertyName falls back to createMissingNode(Identifier)
 * per parser.ts ~4210 when no property-name-starting token is present).
 * `body` is nullable to mirror parseFunctionBlockOrSemicolon, which returns
 * undefined when a semicolon stands in for the body (ambient / overload).
 */
typedef struct {
    CtscNodeArray modifiers;        /* ModifierLike token leaves; may be empty */
    CtscNode*     name;           /* PropertyName (Identifier | ComputedPropertyName | ...) */
    CtscNodeArray parameters;     /* typically empty for getters, one for setters */
    CtscNode*     body;           /* nullable Block */
} CtscAccessorDeclarationData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parsePropertyDeclaration
 * (~7814): the class-member form `(modifiers)? PropertyName (`?` | `!`)?
 * TypeAnnotation? Initializer? `;`?`. Dispatched from
 * parsePropertyOrMethodDeclaration (~7835) when the token following the
 * PropertyName is NOT `(` or `<` (i.e. not a MethodDeclaration). The oracle
 * (harness/src/oracle-ast.ts) has no explicit case for PropertyDeclaration,
 * so it falls through to the default branch which serialises
 * forEachChildInPropertyDeclaration (~536) visits as a single `children`
 * array. forEachChild visits, in order:
 *   modifiers, name, questionToken, exclamationToken, type, initializer.
 *
 * ctsc currently models `name`, `type`, and `initializer` — modifiers,
 * questionToken, and exclamationToken are skipped until a fixture demands
 * them. `type` is populated by parse_type_annotation when the source has
 * a `: Type` annotation (mirrors upstream parsePropertyDeclaration ~7822:
 * `const type = parseTypeAnnotation();`); it is nullable when absent.
 * The 108_parserComputedPropertyName9.ts fixture (`class C { [e]: Type }`)
 * exercises `type`, and 108_parserComputedPropertyName10.ts
 * (`class C { [e] = 1 }`) exercises the bare initializer shape. `name` is
 * non-null (parsePropertyName falls back to createMissingNode(Identifier)
 * per parser.ts ~4210 when no property-name-starting token is present).
 * `initializer` is nullable to mirror parseInitializer returning undefined
 * when there is no `=`.
 */
typedef struct {
    CtscNodeArray modifiers; /* ModifierLike token leaves; may be empty */
    CtscNode* name;        /* PropertyName (Identifier | ComputedPropertyName | ...) */
    CtscNode* type;        /* nullable TypeNode */
    CtscNode* initializer; /* nullable AssignmentExpression */
} CtscPropertyDeclarationData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeParameter (~3955):
 *     TypeParameter: (modifiers)? Identifier (`extends` Type)? (`=` Type)?
 * The oracle (harness/src/oracle-ast.ts) has no explicit case for
 * TypeParameter, so it falls through to the default branch which serialises
 * forEachChildInTypeParameter (parser.ts ~510) visits as a single `children`
 * array. forEachChild visits, in order: modifiers, name, constraint, default,
 * expression. ctsc currently models only `name` — modifiers / constraint /
 * default / expression are skipped until a fixture demands them (the only
 * currently-unlocked fixture, 107_FunctionPropertyAssignments6_es6.ts:
 * `{ *<T>() { } }`, has just the bare identifier).
 */
typedef struct {
    CtscNode* name; /* Identifier */
} CtscTypeParameterData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseClassDeclarationOrExpression
 * (~8154):
 *     parseExpected(ClassKeyword);
 *     const name = parseNameOfClassDeclarationOrExpression();
 *     const typeParameters = parseTypeParameters();
 *     const heritageClauses = parseHeritageClauses();
 *     if (parseExpected(OpenBraceToken)) { members = parseClassMembers();
 *         parseExpected(CloseBraceToken); } else members = createMissingList();
 *
 * Shared between `CTSC_SK_ClassDeclaration` and `CTSC_SK_ClassExpression` —
 * upstream parseClassDeclarationOrExpression produces both shapes from the
 * same code path (parser.ts ~8175 branches only on the final factory call:
 * factory.createClassDeclaration vs factory.createClassExpression). The
 * oracle (harness/src/oracle-ast.ts) has no explicit case for either, so
 * both fall through to the default branch which serializes
 * forEachChildInClassDeclarationOrExpression (~1174) visits as a single
 * `children` array. forEachChild visits, in order:
 *   modifiers, name, typeParameters, heritageClauses, members.
 * ctsc currently models `name`, `typeParameters`, and `members` (ClassElement
 * list is grown alongside fixtures); modifiers / heritageClauses are skipped
 * until a fixture demands them. `name` is nullable because a class expression
 * (or a `class implements ...` ambiguity) may omit it; the 106_parser512084.ts
 * fixture (`class foo {`) has a binding identifier, and the 107_classExpression1
 * .ts fixture (`var v = class C {};`) has one too — but a bare
 * `var v = class {};` would produce a nameless expression.
 *
 * `type_parameters` mirrors upstream's NodeArray<TypeParameterDeclaration>
 * (parser.ts parseTypeParameters ~3987). It is populated by the class header
 * parser for `class C<T> {}` — without it the scanner would leave `<T>` in
 * the token stream and the outer SourceFile loop would try to parse it as a
 * statement, producing stray bytes in the emitter output
 * (fixtures/emitter/from-upstream/107_parserGenericClass1.ts).
 *
 *
 * `modifiers` mirrors upstream's NodeArray<ModifierLike> populated by
 * parseModifiers (parser.ts ~8015) and threaded into
 * parseClassDeclarationOrExpression via parseDeclarationWorker (~7502).
 * Each element is a bare token node (kind = <ModifierKeyword>, no payload)
 * produced by tryParseModifier (~7980) / factoryCreateToken + finishNode.
 * When the source uses a TS-only modifier in statement position (e.g.
 * `protected class C {}` from 108_Protected1.ts), the modifier span is
 * absorbed into the ClassDeclaration's finishNode pos so that
 * ClassDeclaration.pos = full_start of the leading modifier. forEachChild
 * visits modifiers first (before name), which is why ast_json.c emits the
 * modifier leaves at the head of the children array.
 */
typedef struct {
    CtscNodeArray modifiers;        /* ModifierLike token leaves; may be empty */
    CtscNode*     name;             /* nullable Identifier */
    CtscNodeArray type_parameters;  /* TypeParameter list; may be empty */
    CtscNodeArray heritage_clauses; /* HeritageClause list; may be empty */
    CtscNodeArray members;          /* ClassElement list; may be empty */
    /* ClassDeclaration only: `export class` (ClassExpression leaves false). */
    bool          has_export;
} CtscClassDeclarationData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseHeritageClause
 * (~8207):
 *     HeritageClause :
 *       `extends` TypeList
 *       `implements` TypeList
 * where TypeList is parseDelimitedList(HeritageClauseElement,
 * parseExpressionWithTypeArguments). `token` is the leading keyword kind
 * (ExtendsKeyword or ImplementsKeyword) so the JSON emitter can surface it
 * as `token` (tsc's HeritageClause.token). forEachChildInHeritageClause
 * visits each element of `types`; the oracle (harness/src/oracle-ast.ts)
 * has no explicit case for HeritageClause, so it falls through to the
 * default branch which emits `token` plus a `children` array (only when
 * non-empty).
 *
 * For the 108_parserErrorRecovery_ExtendsOrImplementsClause1.ts fixture
 * (`class C extends {\n}`), parseDelimitedList sees `{` and asks
 * isValidHeritageClauseObjectLiteral (parser.ts ~2945). `{` is followed by
 * `}` then EOF, and EOF is not one of Comma/OpenBrace/Extends/Implements,
 * so the `{ }` is treated as the class body, not as a heritage type. The
 * resulting HeritageClause has an empty types list; finishNode's end =
 * scanner.getTokenFullStart() == full_start of `{` (= 35 in that fixture).
 */
typedef struct {
    CtscSyntaxKind token;  /* ExtendsKeyword or ImplementsKeyword */
    CtscNodeArray  types;  /* ExpressionWithTypeArguments list; may be empty */
} CtscHeritageClauseData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseExpressionWithTypeArguments
 * (~8216): `LeftHandSideExpression (TypeArguments)?`. Used as the element
 * type of a HeritageClause.types list. `has_type_arguments` tracks whether
 * a TypeArguments NodeArray was produced (mirrors tsc's
 * factory.createExpressionWithTypeArguments receiving a NodeArray vs
 * undefined). forEachChildInExpressionWithTypeArguments visits `expression`
 * then each type argument; the oracle default branch serializes that as a
 * single `children` array.
 */
typedef struct {
    CtscNode*     expression;
    bool          has_type_arguments;
    CtscNodeArray type_arguments;
} CtscExpressionWithTypeArgumentsData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseEnumDeclaration (~8275):
 *     parseExpected(EnumKeyword);
 *     const name = parseIdentifier();
 *     if (parseExpected(OpenBraceToken)) {
 *         members = parseDelimitedList(EnumMembers, parseEnumMember);
 *         parseExpected(CloseBraceToken);
 *     } else {
 *         members = createMissingList<EnumMember>();
 *     }
 *     return finishNode(factory.createEnumDeclaration(modifiers, name, members), pos);
 *
 * The oracle (harness/src/oracle-ast.ts) has no explicit case for
 * EnumDeclaration so it falls through to the default branch which serialises
 * forEachChildInEnumDeclaration visits as a single `children` array.
 * forEachChild visits, in order: modifiers, name, members. ctsc currently
 * models only `name` and `members`; modifiers are skipped until a fixture
 * demands them. `name` is always present — parseIdentifier falls through to
 * createMissingNode(Identifier) (~2619) when the current token is a reserved
 * word (e.g. `enum void {}` from the 106_parserEnumDeclaration4.ts fixture),
 * producing a zero-width missing Identifier at scanner.getTokenFullStart()
 * with an empty escapedText.
 */
typedef struct {
    CtscNode*     name;     /* Identifier (possibly missing, zero-width) */
    CtscNodeArray members;  /* EnumMember list; may be empty */
    bool          has_export; /* true when `export enum` (parser sets from ExportKeyword) */
} CtscEnumDeclarationData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTypeReference (~4577):
 *     TypeReference : TypeName (`<` TypeList `>`)?
 * TypeName is a (dotted) EntityName stored as `typeName`. When type arguments
 * are present the node carries a TypeArguments NodeArray. The oracle
 * (harness/src/oracle-ast.ts) has no explicit case for TypeReference, so it
 * falls through to the default branch which serialises
 * forEachChildInTypeReference (visits typeName then each typeArgument) as a
 * single `children` array. ast_json.c mirrors that.
 *
 * `typeName` is currently restricted to a plain Identifier (no dotted
 * QualifiedName yet — no fixture exercises it). Grown when needed.
 */
typedef struct {
    CtscNode*     typeName;
    bool          has_type_arguments;
    CtscNodeArray type_arguments;
} CtscTypeReferenceData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseModuleBlock (~8290):
 *     ModuleBlock: "{" Statement* "}"
 * forEachChildInModuleBlock (~988) visits each element of `statements`. The
 * oracle (harness/src/oracle-ast.ts) has no explicit case for ModuleBlock, so
 * it falls through to the default branch which serialises forEachChild's
 * visits as a single `children` array (only when non-empty). An empty
 * `namespace foo { }` body therefore emits `{kind,pos,end}` alone, matching
 * the 108_parserModuleDeclaration6.ts oracle byte-for-byte.
 */
typedef struct {
    CtscNodeArray statements;
} CtscModuleBlockData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts
 * parseModuleOrNamespaceDeclaration (~8303) / parseModuleDeclaration (~8338):
 *     ModuleDeclaration:
 *       (`namespace` | `module`) EntityName (`.` EntityName)* ModuleBlock
 *       `module` StringLiteral (ModuleBlock | `;`)
 *       `global` ModuleBlock                                 (global augmentation)
 * forEachChildInModuleDeclaration (~984) visits modifiers, name, body (in
 * that order). The oracle (harness/src/oracle-ast.ts) has no explicit case
 * for ModuleDeclaration, so it falls through to the default branch which
 * serialises forEachChild's visits as a single `children` array.
 *
 * ctsc currently models only `name` + `body`; modifiers / flags (Namespace
 * vs Module, NestedNamespace, GlobalAugmentation) are not surfaced to the
 * oracle (ts.forEachChild does not emit them either). For `namespace foo {}`
 * the children array is `[Identifier, ModuleBlock]`. Dotted-name namespaces
 * (`namespace a.b {}`) recurse via nested ModuleDeclaration nodes as body,
 * matching tsc's parseModuleOrNamespaceDeclaration (~8308).
 */
typedef struct {
    CtscNode* name; /* Identifier | StringLiteral */
    CtscNode* body; /* nullable: ModuleBlock or nested ModuleDeclaration */
} CtscModuleDeclarationData;

/*
 * Mirrors upstream/TypeScript/src/compiler/types.ts TypeAliasDeclaration and
 * parser.ts parseTypeAliasDeclaration (~8249): modifiers, name, typeParameters, type.
 * forEachChildInTypeAliasDeclaration (parser.ts ~907-912) visits those in order.
 */
typedef struct {
    CtscNodeArray modifiers; /* e.g. ExportKeyword for `export type` */
    CtscNode*   name;
    CtscNodeArray type_parameters;
    CtscNode*   type;
} CtscTypeAliasDeclarationData;

/*
 * Mirrors upstream/TypeScript/src/compiler/parser.ts parseImportDeclaration
 * (~8384) / parseNamedImportsOrExports (~8578) / parseImportOrExportSpecifier
 * (~8604) and types.ts ImportDeclaration / ImportClause / NamedImports /
 * ImportSpecifier.
 */
typedef struct {
    CtscNode* importClause;
    CtscNode* moduleSpecifier;
} CtscImportDeclarationData;

typedef struct {
    bool        is_type_only; /* `import type ...`; mirrors ImportClause.isTypeOnly / phaseModifier TypeKeyword */
    CtscNode*   name;
    CtscNode*   namedBindings;
} CtscImportClauseData;

typedef struct {
    CtscNodeArray elements;
} CtscNamedImportsData;

/*
 * Mirrors upstream types.ts NamespaceImport and parser.ts parseNamespaceImport
 * (~8558): `* as ImportedBinding`.
 */
typedef struct {
    CtscNode* name; /* Identifier */
} CtscNamespaceImportData;

typedef struct {
    bool        is_type_only; /* `import { type X }`; mirrors ImportSpecifier.isTypeOnly */
    CtscNode*   propertyName;
    CtscNode*   name;
} CtscImportSpecifierData;

/*
 * Mirrors upstream types.ts ExportDeclaration (parser.ts parseExportDeclaration ~8701,
 * emitter.ts emitExportDeclaration ~3753).
 */
typedef struct {
    bool      is_type_only; /* `export type { ... } from` */
    CtscNode* export_clause; /* NamedExports | NamespaceExport | NULL for `export * from` */
    CtscNode* module_specifier;
} CtscExportDeclarationData;

typedef struct {
    CtscNodeArray elements;
} CtscNamedExportsData;

typedef CtscImportSpecifierData CtscExportSpecifierData;

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
        CtscThrowStatementData          throwStatement;
        CtscVariableStatementData       variableStatement;
        CtscVariableDeclarationListData variableDeclarationList;
        CtscVariableDeclarationData     variableDeclaration;
        CtscBinaryExpressionData        binaryExpression;
        CtscCallExpressionData          callExpression;
        CtscNewExpressionData           newExpression;
        CtscFunctionDeclarationData     functionDeclaration;
        CtscArrowFunctionData           arrowFunction;
        CtscParameterData               parameter;
        CtscIfStatementData             ifStatement;
        CtscWhileStatementData          whileStatement;
        CtscDoStatementData             doStatement;
        CtscWithStatementData           withStatement;
        CtscSwitchStatementData         switchStatement;
        CtscCaseBlockData               caseBlock;
        CtscCaseClauseData              caseClause;
        CtscDefaultClauseData           defaultClause;
        CtscForStatementData            forStatement;
        CtscForInOrOfStatementData      forInOrOfStatement;
        CtscParenthesizedExpressionData parenthesizedExpression;
        CtscPrefixUnaryExpressionData   prefixUnaryExpression;
        CtscPostfixUnaryExpressionData  postfixUnaryExpression;
        CtscPropertyAccessExpressionData propertyAccessExpression;
        CtscElementAccessExpressionData elementAccessExpression;
        CtscConditionalExpressionData   conditionalExpression;
        CtscObjectLiteralExpressionData objectLiteralExpression;
        CtscPropertyAssignmentData      propertyAssignment;
        CtscShorthandPropertyAssignmentData shorthandPropertyAssignment;
        CtscSpreadAssignmentData        spreadAssignment;
        CtscSpreadElementData           spreadElement;
        CtscComputedPropertyNameData    computedPropertyName;
        CtscArrayLiteralExpressionData  arrayLiteralExpression;
        CtscBreakOrContinueStatementData breakOrContinueStatement;
        CtscLabeledStatementData        labeledStatement;
        CtscTryStatementData            tryStatement;
        CtscCatchClauseData             catchClause;
        CtscVoidExpressionData          voidExpression;
        CtscAwaitExpressionData         awaitExpression;
        CtscYieldExpressionData         yieldExpression;
        CtscMethodDeclarationData       methodDeclaration;
        CtscAccessorDeclarationData     accessorDeclaration;
        CtscPropertyDeclarationData     propertyDeclaration;
        CtscTypeParameterData           typeParameter;
        CtscClassDeclarationData        classDeclaration;
        CtscHeritageClauseData          heritageClause;
        CtscExpressionWithTypeArgumentsData expressionWithTypeArguments;
        CtscEnumDeclarationData         enumDeclaration;
        CtscTypeReferenceData           typeReference;
        CtscBindingPatternData          bindingPattern;
        CtscBindingElementData          bindingElement;
        CtscTemplateExpressionData      templateExpression;
        CtscTemplateSpanData            templateSpan;
        CtscTaggedTemplateExpressionData taggedTemplateExpression;
        CtscModuleBlockData             moduleBlock;
        CtscModuleDeclarationData       moduleDeclaration;
        CtscTypeAssertionExpressionData typeAssertionExpression;
        CtscAsExpressionData            asExpression;
        CtscTypeAliasDeclarationData    typeAliasDeclaration;
        CtscImportDeclarationData       importDeclaration;
        CtscImportClauseData            importClause;
        CtscNamedImportsData            namedImports;
        CtscNamespaceImportData         namespaceImport;
        /* Same shape as NamespaceImport (`* as name` in export-from). */
        CtscNamespaceImportData         namespaceExport;
        CtscImportSpecifierData         importSpecifier;
        CtscExportDeclarationData       exportDeclaration;
        CtscNamedExportsData            namedExports;
        CtscExportSpecifierData         exportSpecifier;
    } data;
};

struct CtscArena;
CtscNode* ctsc_node_new(struct CtscArena* a, CtscSyntaxKind kind, int pos, int end);
void      ctsc_node_array_init(CtscNodeArray* arr);
void      ctsc_node_array_push(CtscNodeArray* arr, struct CtscArena* a, CtscNode* n);

#endif
