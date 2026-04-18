#include "ctsc/parser.h"
#include "ctsc/emitter.h"
#include "ctsc/arena.h"
#include "ctsc/ast.h"
#include "ctsc/buffer.h"
#include "ctsc/utf8.h"
#include <stdio.h>
#include <string.h>

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)

static int expect_emit(const char* src, const char* expected, const char* label, int* failed) {
    CtscArena a; ctsc_arena_init(&a, 64 * 1024);
    size_t src_len = strlen(src);
    CtscParseResult r = ctsc_parse(src, src_len, &a);

    CtscUtf16Buf u16; ctsc_utf16_init(&u16);
    ctsc_utf16_from_utf8(&u16, src, src_len);

    CtscBuffer out; ctsc_buf_init(&out);
    ctsc_emit_js(r.sourceFile, &u16, &out);

    size_t exp_len = strlen(expected);
    int ok = (out.len == exp_len) && memcmp(out.data, expected, exp_len) == 0;
    if (!ok) {
        fprintf(stderr, "FAIL emitter (%s):\n  expected (%zu): ", label, exp_len);
        fwrite(expected, 1, exp_len, stderr);
        fprintf(stderr, "\n  actual   (%zu): ", out.len);
        fwrite(out.data, 1, out.len, stderr);
        fprintf(stderr, "\n");
        (*failed)++;
    }

    ctsc_buf_free(&out);
    ctsc_utf16_free(&u16);
    ctsc_arena_free(&a);
    return ok ? 0 : 1;
}

int test_emitter(void) {
    int failed = 0;

    /*
     * Mirrors upstream transformers/ts.ts visitEnumDeclaration (~1802) +
     * transformEnumBody (~1891). At SourceFile scope the lowering produces:
     *     var <name>;
     *     (function (<name>) {\n<members>\n})(<name> || (<name> = {}));
     * For the 106_parserEnumDeclaration4.ts fixture (`enum void {}`), tsc's
     * createIdentifier branch falls through to createMissingNode(Identifier)
     * (~2619) because `void` is a reserved word — the zero-width name
     * renders as empty, so the IIFE degenerates to `(function () {\n})( || ( = {}));`.
     * The trailing `void {};` is then parsed as a VoidExpression statement
     * (see test_parser enum recovery cases).
     */
    expect_emit(
        "// @target: es2015\nenum void {\n}",
        "// @target: es2015\nvar ;\n(function () {\n})( || ( = {}));\nvoid {};\n",
        "enum void {} (recovery)",
        &failed
    );

    /* Well-formed empty enum: `enum E {}` at SourceFile scope lowers to the
     * same IIFE pattern with `E` in every name slot. */
    expect_emit(
        "enum E {}",
        "var E;\n(function (E) {\n})(E || (E = {}));\n",
        "enum E {}",
        &failed
    );

    /*
     * Mirrors upstream/TypeScript/src/compiler/parser.ts
     * parseFunctionBlockOrSemicolon (~7567) + transformers/ts.ts
     * visitFunctionDeclaration (~1555). A function declaration with no body
     * (overload signature `function foo();`) is replaced by
     * NotEmittedStatement and prints zero bytes — including dropping the
     * preceding non-triple-slash leading comment, since
     * emitLeadingComments(pos=0, isEmittedNode=false) (emitter.ts ~5976)
     * only forwards triple-slash comments. Fixture
     * fixtures/emitter/from-upstream/106_parserFunctionDeclaration3.ts.
     */
    expect_emit(
        "// @target: es2015\nfunction foo();",
        "",
        "function foo(); (overload signature)",
        &failed
    );

    /* And the same shape with a body still round-trips through the normal
     * emitter path. */
    expect_emit(
        "function foo() {}",
        "function foo() { }\n",
        "function foo() {} (with body)",
        &failed
    );

    /* export function: emitDecoratorsAndModifiers writes ExportKeyword before
     * emitFunctionDeclarationOrExpression (upstream emitter.ts ~3430). */
    expect_emit(
        "export function foo(n: number): number {\n  return n + 1;\n}\n",
        "export function foo(n) {\n    return n + 1;\n}\n",
        "export function (types elided)",
        &failed
    );

    /*
     * export const / export class + class field → constructor lowering
     * (fixtures/emitter/selfhost-derived/51_export_const_class.ts).
     */
    expect_emit(
        "export const VERSION: string = \"1.0.0\";\n\nexport class Box {\n  value: number = 0;\n}\n",
        "export const VERSION = \"1.0.0\";\nexport class Box {\n    constructor() {\n        this.value = 0;\n    }\n}\n",
        "export const + export class with property initializer",
        &failed
    );

    /*
     * Mirrors upstream/TypeScript/src/compiler/parser.ts parseFunctionExpression
     * (~6765) + emitter.ts emitFunctionDeclarationOrExpression (~3430).
     * FunctionExpression is a primary expression starter via
     * parsePrimaryExpression's FunctionKeyword case (~6644); without that
     * dispatch `++function(e) { }` parses as a PrefixUnaryExpression over a
     * missing-identifier operand followed by a stray FunctionDeclaration,
     * which the printer emits as `++;\nfunction (e) { }`. The expected
     * byte stream is the prefix-unary applied to the FunctionExpression,
     * with the always-space-after-function convention of
     * emitFunctionDeclarationOrExpression (writeKeyword("function") +
     * writeSpace + emitIdentifierName — an absent name emits nothing, so
     * the space collapses against `(`). Fixture
     * fixtures/emitter/from-upstream/107_parserUnaryExpression2.ts.
     */
    expect_emit(
        "// @target: es2015\n++function(e) { }",
        "// @target: es2015\n++function (e) { };\n",
        "++function(e) { } (anonymous FunctionExpression as ++ operand)",
        &failed
    );

    /*
     * Mirrors upstream emitter.ts emitTryStatement (~3377). The fixture
     * 106_parserMissingToken1.ts (`a / finally` at EOF) exercises the
     * missing-try + bare-finally recovery path: the parser synthesises
     * a zero-width tryBlock and a finallyBlock that consumed `finally`
     * followed by a zero-width missing Block. emitTryStatement writes
     *     try { }\nfinally { }
     * because writeLineOrSpace (~4957) degenerates to writeLine() under
     * ts.transpileModule (preserveSourceNewlines === undefined, no
     * SingleLine emit flag). The preceding `a /` parses as a
     * BinaryExpression with a missing right operand, emitted as `a / `
     * followed by the ExpressionStatement's trailing semicolon.
     */
    expect_emit(
        "// @target: es2015\na / finally",
        "// @target: es2015\na / ;\ntry { }\nfinally { }\n",
        "try/finally recovery (bare finally at EOF)",
        &failed
    );

    /*
     * Mirrors upstream emitter.ts emitClassDeclarationOrExpression (~3557)
     * for ClassExpression: same printer path as ClassDeclaration, an empty
     * members list uses ListFormat.ClassMembers (Indented | MultiLine),
     * which in emitNodeList's isEmpty branch (~4701) writes a single
     * writeLine() before the close brace — yielding `{\n}`. Fixture
     * fixtures/emitter/from-upstream/107_classExpressionES61.ts.
     */
    expect_emit(
        "// @target: es6\nvar v = class C {};",
        "// @target: es6\nvar v = class C {\n};\n",
        "class expression (empty body)",
        &failed
    );

    /* Anonymous class expression: no name after the class keyword. */
    expect_emit(
        "var v = class {};",
        "var v = class {\n};\n",
        "class expression (anonymous, empty body)",
        &failed
    );

    /*
     * Mirrors upstream emitter.ts emitSemicolonClassElement (~2322):
     *     writeTrailingSemicolon();
     * surrounded by the ClassMembers list format (Indented | MultiLine,
     * types.ts ~10177) applied by emitClassDeclarationOrExpression (~3557).
     * Fixture fixtures/emitter/from-upstream/107_classWithSemicolonClassElementES61.ts.
     */
    expect_emit(
        "//@target: es6\nclass C {\n    ;\n}",
        "//@target: es6\nclass C {\n    ;\n}\n",
        "class declaration with SemicolonClassElement",
        &failed
    );

    /*
     * Mirrors upstream parser.ts parseRightSideOfDot (~3613) with
     * allowIdentifierNames=true: the RHS of `.` accepts any
     * identifier-or-keyword. Fixture fixtures/emitter/from-upstream/
     * 107_parser521128.ts (`module.module { }`) parses as a PropertyAccess
     * `module.module` (ExpressionStatement; ASI inserts `;`) followed by an
     * empty Block. The block emits as `{ }` via SingleLineBlockStatements.
     */
    expect_emit(
        "// @target: es2015\nmodule.module { }",
        "// @target: es2015\nmodule.module;\n{ }\n",
        "property access with contextual-keyword name (module.module)",
        &failed
    );

    /*
     * Mirrors upstream/TypeScript/src/compiler/parser.ts
     * parseParenthesizedArrowFunctionExpression (~5430) + parseDelimitedList
     * (~3489, ParsingContext.Parameters) error recovery path, and emitter.ts
     * emitObjectBindingPattern (~2585). The fixture
     * 107_parserErrorRecovery_ParameterList5.ts (`(a:number => { }`) drives
     * this shape:
     *   - Parameter 1 = `a: number` (parse_parameter consumes the type
     *     annotation; scanner advances to `=>`).
     *   - Stray `=>` is not a parameter start / list terminator / statement
     *     start; abortParsingListOrMoveToNextToken advances past it.
     *   - Parameter 2 starts at `{` — isParameterNameStart returns true for
     *     OpenBrace, parse_object_binding_pattern consumes `{}`.
     *   - List ends at EOF. parseExpected(CloseParen) emits ')' expected,
     *     parseExpectedToken(EqualsGreaterThan) emits '=>' expected with a
     *     zero-width equals-greater-than at EOF. lastToken (captured before
     *     parseExpectedToken) is EOF which is neither `=>` nor `{`, so body =
     *     parseIdentifier() synthesises a zero-width missing Identifier.
     *   - Emitter prints ArrowFunction as `(a, {}) => ` + missing-identifier
     *     (empty) + ExpressionStatement's trailing `;`.
     */
    expect_emit(
        "// @target: es2015\n(a:number => { }",
        "// @target: es2015\n(a, {}) => ;\n",
        "parenthesized arrow parameter list error recovery (ParameterList5)",
        &failed
    );

    /*
     * Mirrors upstream/TypeScript/src/compiler/parser.ts
     * isParenthesizedArrowFunctionExpressionWorker (~5294 `(...` always
     * Tristate.True) and parseParameterWorker (~4069 dotDotDotToken, ~4081
     * questionToken). Fixture 107_parserParameterList11.ts
     * (`(...arg?) => 102`): the rest parameter is parsed as
     * `Parameter(..., name=arg, questionToken)`, and transformers/ts.ts
     * visitParameter drops the TS-only `?` before emit, so the JS
     * output is `(...arg) => 102;`.
     */
    expect_emit(
        "// @target: es2015\n(...arg?) => 102;",
        "// @target: es2015\n(...arg) => 102;\n",
        "parenthesized arrow with rest parameter and optional `?` (ParameterList11)",
        &failed
    );

    /*
     * Mirrors upstream/TypeScript/src/compiler/parser.ts isStartOfStatement
     * (~7316-7324) for the TS-only modifier keywords: when a modifier like
     * PublicKeyword / PrivateKeyword / ProtectedKeyword / StaticKeyword /
     * ReadonlyKeyword does NOT start a declaration AND the next token on
     * the same line is an identifier / keyword, isStartOfStatement returns
     * false, parseList (~3094) rejects it in isListElement, and
     * abortParsingListOrMoveToNextToken (~3410) reports
     * Diagnostics.Declaration_or_statement_expected (code 1128) and
     * advances one token. The result is a SourceFile whose statements
     * begin AFTER the modifier — the `public` in `public break;` is not
     * emitted as an ExpressionStatement.
     *
     * Fixture 107_parserPublicBreak1.ts (`// @target: es2015\npublic break;\n`):
     * tsc produces a single BreakStatement at pos 25 (the ` ` before
     * `break`, since that is scanner.getTokenFullStart() when `public`
     * was consumed and then advanced past). The subsequent emitter's
     * leading-comments replay at `first_emitted->pos` = 25 finds no
     * comments (the scan from pos 25 encounters the `break` keyword
     * without crossing any `//`/`\/\*` trivia), so the `// @target:
     * es2015` comment is NOT forwarded — matching tsc's transpileModule
     * output of exactly `break;\n`.
     */
    expect_emit(
        "// @target: es2015\npublic break;\n",
        "break;\n",
        "public-modifier-before-break recovery (parserPublicBreak1)",
        &failed
    );

    /*
     * Mirrors upstream/TypeScript/src/compiler/parser.ts parseDeleteExpression
     * (~5698) + emitter.ts emitDeleteExpression (~2783). The fixture
     * 107_parserUnaryExpression5.ts (`++ delete foo.bar`) parses as two
     * statements because parseUpdateExpression's `++` recursion calls
     * parseLeftHandSideExpressionOrHigher, which does NOT descend into the
     * keyword-unary expressions — so `delete` is not a valid operand there,
     * the primary falls through to createMissingNode(Identifier) and the
     * PrefixUnaryExpression's operand.end equals the full_start of `delete`.
     * ASI then closes the first ExpressionStatement, and the outer parseList
     * starts a fresh statement at `delete`, which goes through parseSimpleUnary
     * Expression -> parseDeleteExpression -> DeleteExpression(PropertyAccess
     * (foo, bar)). Emitter writes `delete ` + operand + trailing `;`.
     */
    expect_emit(
        "// @target: es2015\n++ delete foo.bar",
        "// @target: es2015\n++;\ndelete foo.bar;\n",
        "delete expression after prefix ++ (parserUnaryExpression5)",
        &failed
    );

    /* Same shape for the other keyword-prefix unaries, which ctsc stores in
     * the shared voidExpression data struct and emits with the matching
     * keyword spelling. `typeof` and `void` go through the exact same parser
     * and printer path as `delete`. */
    expect_emit(
        "typeof foo;",
        "typeof foo;\n",
        "typeof expression",
        &failed
    );

    /*
     * Mirrors upstream/TypeScript/src/compiler/parser.ts parseTemplateExpression
     * (~3668), parseTemplateSpan (~3724) and parseLiteralOfTemplateSpan (~3713).
     * Fixture 107_TemplateExpression1.ts (`var v = `foo ${ a `): the scanner
     * returns a TemplateHead at `\`foo ${`, parse_primary builds
     * TemplateExpression(head, [TemplateSpan(Identifier "a", <missing Tail>)]).
     * parseLiteralOfTemplateSpan synthesises a zero-width missing TemplateTail
     * via createMissingNode (~2619) because the current token is EOF, not `}`.
     *
     * Emitter mirrors emitTemplateExpression (~2966) +
     * emitTemplateSpan (~3027) which emit head, then each span's expression
     * followed by its literal; the missing tail contributes zero bytes, so the
     * JS output is `var v = `foo ${a;\n`.
     */
    expect_emit(
        "// @target: es2015\nvar v = `foo ${ a ",
        "// @target: es2015\nvar v = `foo ${a;\n",
        "template expression with missing tail (TemplateExpression1)",
        &failed
    );

    /*
     * Mirrors upstream parser.ts parseThrowStatement (~7053) +
     * emitter.ts emitThrowStatement (~3371). parse_type_annotation must treat
     * `never` like other keyword types (parseKeywordAndNoDot) so `: never {`
     * does not swallow the function body (selfhost-derived 52_throw_statement.ts).
     */
    expect_emit(
        "function f(): never { throw 1; }\n"
        "function g(x: unknown): void {\n  if (x) { throw new Error(\"e\"); }\n}",
        "function f() {\n    throw 1;\n}\n"
        "function g(x) {\n    if (x) {\n        throw new Error(\"e\");\n    }\n}\n",
        "throw statement + never / unknown return types",
        &failed
    );

    /*
     * Mirrors parser.ts parseSwitchStatement (~7042) + emitter.ts
     * emitSwitchStatement (~3354) / emitCaseBlock (~3657) / emitCaseClause
     * (~3994) / emitDefaultClause (~4002). Numeric separators use scanner.ts
     * scanNumberFragment (~1171); emitter uses canonical tokenValue (fixtures/
     * emitter/selfhost-derived/53_switch_statement.ts).
     */
    expect_emit(
        "function unitToMs(unit: string, n: number): number {\n"
        "  switch (unit) {\n"
        "    case \"s\":\n"
        "      return n * 1000;\n"
        "    case \"m\":\n"
        "      return n * 60_000;\n"
        "    case \"h\":\n"
        "      return n * 3_600_000;\n"
        "    default:\n"
        "      throw new Error(\"unknown unit: \" + unit);\n"
        "  }\n"
        "}\n",
        "function unitToMs(unit, n) {\n"
        "    switch (unit) {\n"
        "        case \"s\":\n"
        "            return n * 1000;\n"
        "        case \"m\":\n"
        "            return n * 60000;\n"
        "        case \"h\":\n"
        "            return n * 3600000;\n"
        "        default:\n"
        "            throw new Error(\"unknown unit: \" + unit);\n"
        "    }\n"
        "}\n",
        "switch statement + numeric separators",
        &failed
    );

    /*
     * utilities.ts escapeString (~6207) + doubleQuoteEscapedCharsRegExp (~6160);
     * fixture emitter/selfhost-derived/54_string_escapes.ts.
     */
    expect_emit(
        "const backslash: string = \"\\\\\";\n"
        "const doubleBackslash: string = \"\\\\\\\\\";\n"
        "const dollarRef: string = \"\\\\$&\";\n"
        "const hexEscape: string = \"\\\\x2d\";\n"
        "const newlineLit: string = \"line1\\nline2\";\n"
        "const tabLit: string = \"col1\\tcol2\";\n"
        "const quoteInStr: string = \"she said \\\"hi\\\"\";\n",
        "const backslash = \"\\\\\";\n"
        "const doubleBackslash = \"\\\\\\\\\";\n"
        "const dollarRef = \"\\\\$&\";\n"
        "const hexEscape = \"\\\\x2d\";\n"
        "const newlineLit = \"line1\\nline2\";\n"
        "const tabLit = \"col1\\tcol2\";\n"
        "const quoteInStr = \"she said \\\"hi\\\"\";\n",
        "string literal escape re-emission",
        &failed
    );

    return failed;
}
