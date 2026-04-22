#ifndef CTSC_SCANNER_H
#define CTSC_SCANNER_H

#include "common.h"
#include "utf8.h"
#include "diagnostic.h"
#include "buffer.h"

/*
 * Subset of ts.SyntaxKind. Values are stable within ctsc but not intended to
 * match TypeScript's numeric values. Callers should emit the *name* via
 * ctsc_syntax_kind_name() to keep byte-exact parity with tsc's JSON oracle.
 *
 * The set is grown by the agent loop as fixtures demand new token kinds.
 */
typedef enum {
    CTSC_SK_Unknown = 0,
    CTSC_SK_EndOfFileToken,
    CTSC_SK_SingleLineCommentTrivia,
    CTSC_SK_MultiLineCommentTrivia,
    CTSC_SK_NewLineTrivia,
    CTSC_SK_WhitespaceTrivia,

    /* Literals */
    CTSC_SK_NumericLiteral,
    CTSC_SK_BigIntLiteral,
    CTSC_SK_StringLiteral,
    CTSC_SK_RegularExpressionLiteral,
    CTSC_SK_NoSubstitutionTemplateLiteral,
    CTSC_SK_TemplateHead,
    CTSC_SK_TemplateMiddle,
    CTSC_SK_TemplateTail,

    /* Punctuation */
    CTSC_SK_OpenBraceToken,
    CTSC_SK_CloseBraceToken,
    CTSC_SK_OpenParenToken,
    CTSC_SK_CloseParenToken,
    CTSC_SK_OpenBracketToken,
    CTSC_SK_CloseBracketToken,
    CTSC_SK_DotToken,
    CTSC_SK_DotDotDotToken,
    CTSC_SK_SemicolonToken,
    CTSC_SK_CommaToken,
    CTSC_SK_QuestionDotToken,
    CTSC_SK_LessThanToken,
    CTSC_SK_LessThanSlashToken,
    CTSC_SK_GreaterThanToken,
    CTSC_SK_LessThanEqualsToken,
    CTSC_SK_GreaterThanEqualsToken,
    CTSC_SK_EqualsEqualsToken,
    CTSC_SK_ExclamationEqualsToken,
    CTSC_SK_EqualsEqualsEqualsToken,
    CTSC_SK_ExclamationEqualsEqualsToken,
    CTSC_SK_EqualsGreaterThanToken,
    CTSC_SK_PlusToken,
    CTSC_SK_MinusToken,
    CTSC_SK_AsteriskToken,
    CTSC_SK_AsteriskAsteriskToken,
    CTSC_SK_SlashToken,
    CTSC_SK_PercentToken,
    CTSC_SK_PlusPlusToken,
    CTSC_SK_MinusMinusToken,
    CTSC_SK_LessThanLessThanToken,
    CTSC_SK_GreaterThanGreaterThanToken,
    CTSC_SK_GreaterThanGreaterThanGreaterThanToken,
    CTSC_SK_AmpersandToken,
    CTSC_SK_BarToken,
    CTSC_SK_CaretToken,
    CTSC_SK_ExclamationToken,
    CTSC_SK_TildeToken,
    CTSC_SK_AmpersandAmpersandToken,
    CTSC_SK_BarBarToken,
    CTSC_SK_QuestionToken,
    CTSC_SK_ColonToken,
    CTSC_SK_AtToken,
    CTSC_SK_QuestionQuestionToken,
    CTSC_SK_EqualsToken,
    CTSC_SK_PlusEqualsToken,
    CTSC_SK_MinusEqualsToken,
    CTSC_SK_AsteriskEqualsToken,
    CTSC_SK_SlashEqualsToken,
    CTSC_SK_PercentEqualsToken,
    CTSC_SK_GreaterThanGreaterThanEqualsToken,
    CTSC_SK_GreaterThanGreaterThanGreaterThanEqualsToken,

    /* Identifiers & keywords */
    CTSC_SK_Identifier,
    CTSC_SK_PrivateIdentifier,

    /* Reserved words (subset; expanded by agent) */
    CTSC_SK_BreakKeyword,
    CTSC_SK_CaseKeyword,
    CTSC_SK_CatchKeyword,
    CTSC_SK_ClassKeyword,
    CTSC_SK_ConstKeyword,
    CTSC_SK_ContinueKeyword,
    CTSC_SK_DebuggerKeyword,
    CTSC_SK_DefaultKeyword,
    CTSC_SK_DeleteKeyword,
    CTSC_SK_DoKeyword,
    CTSC_SK_ElseKeyword,
    CTSC_SK_EnumKeyword,
    CTSC_SK_ExportKeyword,
    CTSC_SK_ExtendsKeyword,
    CTSC_SK_FalseKeyword,
    CTSC_SK_FinallyKeyword,
    CTSC_SK_ForKeyword,
    CTSC_SK_FunctionKeyword,
    CTSC_SK_IfKeyword,
    CTSC_SK_ImportKeyword,
    CTSC_SK_InKeyword,
    CTSC_SK_InstanceOfKeyword,
    CTSC_SK_NewKeyword,
    CTSC_SK_NullKeyword,
    CTSC_SK_ReturnKeyword,
    CTSC_SK_SuperKeyword,
    CTSC_SK_SwitchKeyword,
    CTSC_SK_ThisKeyword,
    CTSC_SK_ThrowKeyword,
    CTSC_SK_TrueKeyword,
    CTSC_SK_TryKeyword,
    CTSC_SK_TypeOfKeyword,
    CTSC_SK_VarKeyword,
    CTSC_SK_VoidKeyword,
    CTSC_SK_WhileKeyword,
    CTSC_SK_WithKeyword,

    /* Contextual keywords */
    CTSC_SK_AsKeyword,
    CTSC_SK_AsyncKeyword,
    CTSC_SK_AwaitKeyword,
    CTSC_SK_ConstructorKeyword,
    CTSC_SK_FromKeyword,
    CTSC_SK_GetKeyword,
    CTSC_SK_LetKeyword,
    CTSC_SK_OfKeyword,
    CTSC_SK_SetKeyword,
    CTSC_SK_YieldKeyword,

    /* TS-specific keywords */
    CTSC_SK_AbstractKeyword,
    CTSC_SK_AnyKeyword,
    CTSC_SK_BooleanKeyword,
    CTSC_SK_DeclareKeyword,
    CTSC_SK_InterfaceKeyword,
    CTSC_SK_ImplementsKeyword,
    CTSC_SK_ModuleKeyword,
    CTSC_SK_NamespaceKeyword,
    CTSC_SK_NumberKeyword,
    CTSC_SK_ObjectKeyword,
    CTSC_SK_PrivateKeyword,
    CTSC_SK_ProtectedKeyword,
    CTSC_SK_PublicKeyword,
    CTSC_SK_ReadonlyKeyword,
    CTSC_SK_StaticKeyword,
    CTSC_SK_StringKeyword,
    CTSC_SK_SymbolKeyword,
    CTSC_SK_TypeKeyword,
    CTSC_SK_UndefinedKeyword,
    CTSC_SK_NeverKeyword,
    /*
     * Contextual type-operator keyword. Mirrors ts.SyntaxKind.KeyOfKeyword
     * (upstream/TypeScript/src/compiler/types.ts), consumed only inside a
     * type position as the `keyof` prefix of a TypeOperatorNode (parser.ts
     * parseTypeOperatorOrHigher ~4781 / parseTypeOperator ~4752). Outside
     * type positions the parser accepts it as an ordinary Identifier via
     * the contextual-keyword range check in is_binding_identifier_kind /
     * token_is_identifier_expression (AsKeyword..UnknownKeyword).
     *
     * NOTE: placed before UnknownKeyword so the existing contextual-keyword
     * range checks pick it up without changes. Any new TS keyword kind must
     * be inserted inside that range to preserve identifier-fallback
     * semantics (selfhost `keyof` as a variable / property name etc.).
     */
    CTSC_SK_KeyOfKeyword,
    CTSC_SK_UnknownKeyword,

    /* Node kinds (Phase 2+; agent 扩展时按 tsc ts.SyntaxKind 添加更多) */
    CTSC_SK_SourceFile,
    CTSC_SK_Block,
    CTSC_SK_VariableStatement,
    CTSC_SK_VariableDeclarationList,
    CTSC_SK_VariableDeclaration,
    CTSC_SK_ExpressionStatement,
    CTSC_SK_EmptyStatement,
    CTSC_SK_IfStatement,
    CTSC_SK_WhileStatement,
    CTSC_SK_ForStatement,
    CTSC_SK_ForInStatement,
    CTSC_SK_ForOfStatement,
    CTSC_SK_DoStatement,
    CTSC_SK_BreakStatement,
    CTSC_SK_ContinueStatement,
    CTSC_SK_DebuggerStatement,
    CTSC_SK_TryStatement,
    CTSC_SK_WithStatement,
    CTSC_SK_SwitchStatement,
    CTSC_SK_CaseBlock,
    CTSC_SK_CaseClause,
    CTSC_SK_DefaultClause,
    CTSC_SK_ParenthesizedExpression,
    CTSC_SK_ConditionalExpression,
    CTSC_SK_ReturnStatement,
    CTSC_SK_ThrowStatement,
    CTSC_SK_FunctionDeclaration,
    CTSC_SK_FunctionExpression,
    CTSC_SK_Parameter,
    CTSC_SK_BinaryExpression,
    CTSC_SK_PrefixUnaryExpression,
    CTSC_SK_PostfixUnaryExpression,
    CTSC_SK_CallExpression,
    CTSC_SK_NewExpression,
    CTSC_SK_PropertyAccessExpression,
    CTSC_SK_ElementAccessExpression,
    CTSC_SK_ArrayLiteralExpression,
    CTSC_SK_ObjectLiteralExpression,
    CTSC_SK_PropertyAssignment,
    CTSC_SK_ShorthandPropertyAssignment,
    CTSC_SK_ComputedPropertyName,
    CTSC_SK_MethodDeclaration,
    CTSC_SK_GetAccessor,
    CTSC_SK_SetAccessor,
    CTSC_SK_PropertyDeclaration,
    CTSC_SK_SemicolonClassElement,
    CTSC_SK_ClassDeclaration,
    CTSC_SK_ClassExpression,
    CTSC_SK_InterfaceDeclaration,
    CTSC_SK_TypeAliasDeclaration,
    CTSC_SK_HeritageClause,
    CTSC_SK_ExpressionWithTypeArguments,
    CTSC_SK_EnumDeclaration,
    CTSC_SK_EnumMember,
    CTSC_SK_ArrowFunction,
    CTSC_SK_TypeReference,
    CTSC_SK_TypeLiteral,
    CTSC_SK_TypeParameter,
    CTSC_SK_PropertySignature,
    CTSC_SK_VoidExpression,
    CTSC_SK_DeleteExpression,
    CTSC_SK_TypeOfExpression,
    CTSC_SK_YieldExpression,
    CTSC_SK_OmittedExpression,
    CTSC_SK_ObjectBindingPattern,
    CTSC_SK_ArrayBindingPattern,
    CTSC_SK_BindingElement,
    CTSC_SK_TemplateExpression,
    CTSC_SK_TemplateSpan,
    CTSC_SK_TaggedTemplateExpression,
    CTSC_SK_ModuleDeclaration,
    CTSC_SK_ModuleBlock,
    CTSC_SK_TypeAssertionExpression,
    CTSC_SK_AsExpression,
    CTSC_SK_AwaitExpression,
    CTSC_SK_SpreadAssignment,
    CTSC_SK_SpreadElement,
    CTSC_SK_ImportDeclaration,
    CTSC_SK_ImportClause,
    CTSC_SK_NamedImports,
    CTSC_SK_NamespaceImport,
    CTSC_SK_ImportSpecifier,
    /* Mirrors ts.SyntaxKind.ExportDeclaration / NamedExports / ExportSpecifier /
     * NamespaceExport (parser.ts parseExportDeclaration ~8701). */
    CTSC_SK_ExportDeclaration,
    CTSC_SK_NamedExports,
    CTSC_SK_ExportSpecifier,
    CTSC_SK_NamespaceExport,
    /* Mirrors ts.SyntaxKind.CatchClause (forEachChild: variableDeclaration, block). */
    CTSC_SK_CatchClause,
    /* Mirrors ts.SyntaxKind.LabeledStatement (parser.ts parseExpressionOrLabeledStatement ~7123). */
    CTSC_SK_LabeledStatement,
    /* Mirrors ts.SyntaxKind.TypeQuery (types.ts ~254 / parser.ts parseTypeQuery ~3946):
     * `typeof E` in a type position, where E is an EntityName. ctsc currently
     * supports only the Identifier form (single-name); dotted exprName and
     * type arguments are not yet modelled — fixtures will add them. */
    CTSC_SK_TypeQuery,
    /*
     * Mirrors ts.SyntaxKind.TypeOperator (upstream/TypeScript/src/compiler/
     * parser.ts parseTypeOperator ~4752, types.ts TypeOperatorNode). A unary
     * prefix in type position: `keyof T`, `readonly T`, `unique symbol`.
     * ctsc currently only emits this node for `keyof`; the `readonly`
     * variant is still consumed inline by the tuple path (see
     * consume_postfix_type_operators) and `unique symbol` isn't modelled.
     * The operand TypeNode is stored in typeReference.typeName (reusing the
     * single-child slot to keep the union-data layout unchanged), and the
     * operator kind is recorded in typeReference.has_type_arguments=false
     * plus the enclosing node's kind.
     */
    CTSC_SK_TypeOperator,
    /*
     * Mirrors ts.SyntaxKind.IndexedAccessType (upstream/TypeScript/src/
     * compiler/types.ts IndexedAccessTypeNode; parser.ts
     * parsePostfixTypeOrHigher ~4716 wraps the base type when `[` is
     * followed by a non-empty type instead of `]`). checker.ts
     * getTypeFromIndexedAccessTypeNode ~19722:
     *     const objectType = getTypeFromTypeNode(node.objectType);
     *     const indexType = getTypeFromTypeNode(node.indexType);
     *     links.resolvedType = getIndexedAccessType(objectType, indexType, ...);
     *
     * ctsc stores the two operands inside the shared
     * CtscTypeReferenceData slot (no dedicated struct yet): `typeName`
     * is the objectType TypeNode, `type_arguments[0]` is the indexType
     * TypeNode, and `has_type_arguments = true`. This keeps the union
     * data layout unchanged while the M4.x indexed-access slice lands.
     */
    CTSC_SK_IndexedAccessType,
    /*
     * Mirrors ts.SyntaxKind.ConditionalType (upstream/TypeScript/src/compiler/
     * types.ts ConditionalTypeNode; parser.ts parseConditionalTypeOrHigher
     * ~5002):
     *     checkType extends extendsType ? trueType : falseType
     * Stored with four child TypeNodes in CtscConditionalTypeData
     * (data.conditionalType). The checker reduces a non-generic
     * `checkType extends extendsType` to either `trueType` or `falseType`
     * by invoking the structural assignability relation (checker.ts
     * getConditionalType ~17942 / isTypeAssignableTo).
     */
    CTSC_SK_ConditionalType,

    CTSC_SK__COUNT
} CtscSyntaxKind;

const char* ctsc_syntax_kind_name(CtscSyntaxKind k);
CtscSyntaxKind ctsc_identifier_or_keyword(const uint16_t* text, size_t len);

typedef struct {
    CtscSyntaxKind kind;
    int full_start; /* offset of leading trivia (= previous token end). */
    int start;      /* UTF-16 code unit offset, inclusive */
    int end;        /* UTF-16 code unit offset, exclusive */
    /*
     * For identifiers / literals, the textual content as it appears in source.
     * For string / template literals, the *decoded* value (escapes processed).
     * NULL when not applicable.
     */
    const uint16_t* text;       /* into source or arena */
    size_t          text_len;
    const uint16_t* value;      /* for strings: decoded value */
    size_t          value_len;
    bool            has_preceding_line_break;
    /*
     * True iff this numeric literal carries TokenFlags.IsInvalid per
     * upstream/TypeScript/src/compiler/scanner.ts scanNumber (~1258 Octal,
     * ~1305 ContainsLeadingZero). Mirrors utilities.ts canUseOriginalText
     * (~2036) which disables source-text reuse for those literals so the
     * emitter falls back to the canonical tokenValue.
     */
    bool            numeric_literal_is_invalid;
} CtscToken;

typedef struct {
    CtscUtf16Buf         source;       /* owned */
    size_t               pos;
    bool                 include_trivia;
    CtscDiagnosticList*  diagnostics;  /* borrowed, optional */
    bool                 precedingLineBreak;
    CtscToken            current;
    /* arena for decoded string values */
    void*                arena_ptr;    /* opaque (CtscArena*) to avoid include cycle */
} CtscScanner;

struct CtscArena;
void ctsc_scanner_init(CtscScanner* s, const char* src, size_t len, struct CtscArena* arena, CtscDiagnosticList* diags);
void ctsc_scanner_free(CtscScanner* s);
CtscSyntaxKind ctsc_scanner_next(CtscScanner* s);

/*
 * When '/' was lexed as SlashToken or '/=' as SlashEqualsToken, re-scan as a
 * RegularExpressionLiteral if it begins a primary expression (mirrors
 * upstream scanner.ts reScanSlashToken, parser.ts parsePrimaryExpression).
 */
CtscSyntaxKind ctsc_scanner_re_scan_slash_token(CtscScanner* s);

/*
 * When '>' was lexed as a single GreaterThanToken, merge it with the
 * following character(s) to produce '>=', '>>', '>>>', '>>=' or '>>>='
 * tokens. The scanner intentionally emits a single '>' to preserve
 * nested-generic parsability (e.g. `Array<Array<T>>`); binary-expression /
 * assignment parsing calls this to recover the combined operator. Mirrors
 * upstream/TypeScript/src/compiler/scanner.ts reScanGreaterToken (~2438).
 */
CtscSyntaxKind ctsc_scanner_re_scan_greater_token(CtscScanner* s);

/*
 * When a CloseBraceToken was lexed inside a template expression's
 * substitution (e.g. the `}` after `${expr` in `` `abc${0}def` ``), unscan
 * it and rescan the character sequence as a TemplateMiddle or TemplateTail.
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts reScanTemplateToken
 * (~3658): pos = tokenStart; scanTemplateAndSetTokenValue(...).
 *
 * Callers (parser.ts parseLiteralOfTemplateSpan ~3713) invoke this only
 * when the current token is CloseBraceToken; the scanner expects pos to
 * point at the `}` after unwinding, and scanTemplateAndSetTokenValue then
 * consumes everything up to the next `` ` `` or `${`.
 */
CtscSyntaxKind ctsc_scanner_re_scan_template_token(CtscScanner* s);

/* Dump all tokens (skipping trivia by default) as a JSON document written to `out`. */
void ctsc_scanner_dump_tokens_json(const char* src, size_t len, CtscBuffer* out, bool pretty);

#endif
