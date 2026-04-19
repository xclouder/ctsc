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
     * transformers/ts.ts transformEnumBody (~1891) + transformEnumMember (~1913).
     * Matches fixtures/emitter/selfhost-derived/59_enum_auto_increment.ts (tsc
     * transpileModule).
     */
    expect_emit(
        "enum Direction {\n"
        "  North,\n"
        "  East,\n"
        "  South,\n"
        "  West,\n"
        "}\n"
        "\n"
        "enum HttpStatus {\n"
        "  OK = 200,\n"
        "  NotFound = 404,\n"
        "  ServerError = 500,\n"
        "}\n"
        "\n"
        "const d: Direction = Direction.East;\n"
        "const code: HttpStatus = HttpStatus.OK;\n",
        "var Direction;\n"
        "(function (Direction) {\n"
        "    Direction[Direction[\"North\"] = 0] = \"North\";\n"
        "    Direction[Direction[\"East\"] = 1] = \"East\";\n"
        "    Direction[Direction[\"South\"] = 2] = \"South\";\n"
        "    Direction[Direction[\"West\"] = 3] = \"West\";\n"
        "})(Direction || (Direction = {}));\n"
        "var HttpStatus;\n"
        "(function (HttpStatus) {\n"
        "    HttpStatus[HttpStatus[\"OK\"] = 200] = \"OK\";\n"
        "    HttpStatus[HttpStatus[\"NotFound\"] = 404] = \"NotFound\";\n"
        "    HttpStatus[HttpStatus[\"ServerError\"] = 500] = \"ServerError\";\n"
        "})(HttpStatus || (HttpStatus = {}));\n"
        "const d = Direction.East;\n"
        "const code = HttpStatus.OK;\n",
        "enum members (auto + explicit)",
        &failed
    );

    /*
     * `export enum` → `export var` + IIFE (fixtures/emitter/selfhost-derived/
     * 71_export_enum.ts). Mirrors addVarForEnumOrModuleDeclaration (~2018) +
     * printer VariableStatement with ExportKeyword.
     */
    expect_emit(
        "export enum Color {\n"
        "  Red = 1,\n"
        "  Blue = 2,\n"
        "}\n",
        "export var Color;\n"
        "(function (Color) {\n"
        "    Color[Color[\"Red\"] = 1] = \"Red\";\n"
        "    Color[Color[\"Blue\"] = 2] = \"Blue\";\n"
        "})(Color || (Color = {}));\n",
        "export enum (var + IIFE)",
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
     * parser.ts parseFunctionDeclaration (~7743) parseTypeParameters +
     * emitFunctionDeclarationOrExpression (~3430): type parameters and
     * parameter/return types are elided in JS output. Fixture
     * fixtures/emitter/selfhost-derived/57_generic_constraint.ts.
     */
    expect_emit(
        "function identity<T>(x: T): T {\n  return x;\n}\n\n"
        "function applyTwice<T extends () => void>(fn: T): void {\n  fn();\n  fn();\n}\n\n"
        "function pair<A, B>(a: A, b: B): [A, B] {\n  return [a, b];\n}\n",
        "function identity(x) {\n    return x;\n}\n"
        "function applyTwice(fn) {\n    fn();\n    fn();\n}\n"
        "function pair(a, b) {\n    return [a, b];\n}\n",
        "generic function declarations (type params elided)",
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
     * Mirrors upstream parser.ts parseCatchClause (~7097) + emitter.ts
     * emitCatchClause (~4036). `instanceof` is a relational operator
     * (getBinaryOperatorPrecedence ~5990); op_text maps InstanceOfKeyword.
     */
    expect_emit(
        "// @target: es2020\ntry { 1; } catch (e: unknown) { if (e instanceof Error) { 2; } }",
        "// @target: es2020\ntry {\n    1;\n}\ncatch (e) {\n    if (e instanceof Error) {\n        2;\n    }\n}\n",
        "try/catch with typed binding and instanceof",
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
     * Mirrors upstream parser.ts isStartOfParameter (~3993 DotDotDotToken) +
     * parseDelimitedList Parameters: the function-declaration parameter loop
     * must use the same predicate as the parenthesized-arrow loop so
     * `...name` is parsed as one Parameter (emitParameter writes `...` via
     * has_dot_dot_dot). Selfhost-derived 60_rest_parameters.ts.
     */
    expect_emit(
        "function sum(...nums: number[]): number {\n  return 0;\n}\n",
        "function sum(...nums) {\n    return 0;\n}\n",
        "function declaration with rest parameter",
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
     * Mirrors parser.ts parseTaggedTemplateRest (~6505) + emitter.ts
     * emitTaggedTemplateExpression (~2722). Tag and template are one
     * expression; the printer inserts a space between the tag and the opening
     * backtick (selfhost-derived 82_tagged_template.ts).
     */
    expect_emit(
        "const greeting: string = raw`hello ${\"world\"}`;",
        "const greeting = raw `hello ${\"world\"}`;\n",
        "tagged template expression",
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

    /*
     * transformers/ts.ts transformConstructorBody (~1382) + parseParameterWorker
     * modifiers (parser.ts ~4044). Parameter properties lower to this.x = x in
     * the constructor (fixtures/emitter/selfhost-derived/55_parameter_properties.ts).
     */
    expect_emit(
        "class Point {\n"
        "  constructor(public x: number, public y: number) {}\n"
        "\n"
        "  translate(dx: number, dy: number): Point {\n"
        "    return new Point(this.x + dx, this.y + dy);\n"
        "  }\n"
        "}\n",
        "class Point {\n"
        "    constructor(x, y) {\n"
        "        this.x = x;\n"
        "        this.y = y;\n"
        "    }\n"
        "    translate(dx, dy) {\n"
        "        return new Point(this.x + dx, this.y + dy);\n"
        "    }\n"
        "}\n",
        "constructor parameter properties",
        &failed
    );

    /*
     * parser.ts parseAwaitExpression (~5726) + emitAwaitExpression (~2801) +
     * emitFunctionDeclarationOrExpression (~3430) with AsyncKeyword.
     * Mirrors fixtures/emitter/selfhost-derived/58_async_await.ts (tsc ES2020).
     */
    expect_emit(
        "async function fetchNumber(): Promise<number> {\n"
        "  return 42;\n"
        "}\n"
        "\n"
        "async function doubleIt(n: number): Promise<number> {\n"
        "  const base = await fetchNumber();\n"
        "  return base + n;\n"
        "}\n"
        "\n"
        "const handler = async (x: number): Promise<string> => {\n"
        "  const v = await doubleIt(x);\n"
        "  return \"n=\" + v;\n"
        "};\n",
        "async function fetchNumber() {\n"
        "    return 42;\n"
        "}\n"
        "async function doubleIt(n) {\n"
        "    const base = await fetchNumber();\n"
        "    return base + n;\n"
        "}\n"
        "const handler = async (x) => {\n"
        "    const v = await doubleIt(x);\n"
        "    return \"n=\" + v;\n"
        "};\n",
        "async function, await, async arrow",
        &failed
    );

    /*
     * parser.ts parseMemberExpressionRest (~6457) optional `?.` + emitPropertyAccess/
     * emitElementAccess (emitter.ts ~2638/~2689) + `??` at the same precedence
     * as `||` (utilities.ts ~5754-5759). Selfhost-derived 61_optional_chain.ts.
     */
    expect_emit(
        "interface Nested {\n"
        "  inner?: {\n"
        "    value?: number;\n"
        "    name?: string;\n"
        "  };\n"
        "}\n"
        "\n"
        "function readValue(obj: Nested | null | undefined): number {\n"
        "  return obj?.inner?.value ?? 0;\n"
        "}\n"
        "\n"
        "function readName(obj: Nested | null | undefined): string {\n"
        "  return obj?.inner?.name ?? \"unknown\";\n"
        "}\n"
        "\n"
        "function firstChar(s: string | undefined | null): string {\n"
        "  return s?.[0] ?? \"\";\n"
        "}\n",
        "function readValue(obj) {\n"
        "    return obj?.inner?.value ?? 0;\n"
        "}\n"
        "function readName(obj) {\n"
        "    return obj?.inner?.name ?? \"unknown\";\n"
        "}\n"
        "function firstChar(s) {\n"
        "    return s?.[0] ?? \"\";\n"
        "}\n",
        "optional chaining and nullish coalescing",
        &failed
    );

    /*
     * parse_type_annotation: after a keyword type and postfix `[]`, a following
     * `|` starts a union and must not end the annotation early (parser.ts
     * parseUnionType ~4876). Selfhost-derived 75_export_optional_chain.ts
     * (`arr: string[] | undefined`).
     */
    expect_emit(
        "export function firstLen(arr: string[] | undefined): number {\n"
        "  return arr?.[0]?.length ?? 0;\n"
        "}\n",
        "export function firstLen(arr) {\n"
        "    return arr?.[0]?.length ?? 0;\n"
        "}\n",
        "parameter type union after postfix array (75_export_optional_chain)",
        &failed
    );

    /*
     * Object spread: parser.ts parseObjectLiteralElement (~6703) +
     * emitter.ts emitSpreadAssignment (~4081). Selfhost-derived 64_object_spread.ts.
     */
    expect_emit(
        "interface Config {\n"
        "  host: string;\n"
        "  port: number;\n"
        "}\n"
        "\n"
        "function withDefaults(partial: Partial<Config>): Config {\n"
        "  const defaults: Config = { host: \"localhost\", port: 80 };\n"
        "  return { ...defaults, ...partial };\n"
        "}\n"
        "\n"
        "function merge(...objs: object[]): object {\n"
        "  let out = {};\n"
        "  for (const o of objs) {\n"
        "    out = { ...out, ...o };\n"
        "  }\n"
        "  return out;\n"
        "}\n",
        "function withDefaults(partial) {\n"
        "    const defaults = { host: \"localhost\", port: 80 };\n"
        "    return { ...defaults, ...partial };\n"
        "}\n"
        "function merge(...objs) {\n"
        "    let out = {};\n"
        "    for (const o of objs) {\n"
        "        out = { ...out, ...o };\n"
        "    }\n"
        "    return out;\n"
        "}\n",
        "object literal spread (SpreadAssignment)",
        &failed
    );

    /*
     * SpreadElement in calls + parenthesised function return type + `this`
     * parameter. parser.ts parseArgumentExpression (~6685), emitter.ts
     * emitSpreadExpression (~4071); parse_type_annotation fallback for
     * `): (...) => void {`. Selfhost-derived 74_export_generic_rest.ts.
     */
    expect_emit(
        "export function concat<T>(...arrs: T[][]): T[] {\n"
        "  const out: T[] = [];\n"
        "  for (const a of arrs) {\n"
        "    out.push(...a);\n"
        "  }\n"
        "  return out;\n"
        "}\n"
        "\n"
        "export function debounce<T extends (...args: any[]) => void>(\n"
        "  fn: T,\n"
        "  wait: number,\n"
        "): (...args: Parameters<T>) => void {\n"
        "  let timer: ReturnType<typeof setTimeout> | null = null;\n"
        "  return function (this: unknown, ...args: Parameters<T>): void {\n"
        "    if (timer) clearTimeout(timer);\n"
        "    timer = setTimeout(() => fn.apply(this, args), wait);\n"
        "  };\n"
        "}\n",
        "export function concat(...arrs) {\n"
        "    const out = [];\n"
        "    for (const a of arrs) {\n"
        "        out.push(...a);\n"
        "    }\n"
        "    return out;\n"
        "}\n"
        "export function debounce(fn, wait) {\n"
        "    let timer = null;\n"
        "    return function (...args) {\n"
        "        if (timer)\n"
        "            clearTimeout(timer);\n"
        "        timer = setTimeout(() => fn.apply(this, args), wait);\n"
        "    };\n"
        "}\n",
        "SpreadElement + generic rest / debounce (74_export_generic_rest)",
        &failed
    );

    /*
     * Array / object destructuring: emitter.ts emitBindingElement (~2597) +
     * emitArrayBindingPattern (~2591). Selfhost-derived 65_array_destructure.ts.
     */
    expect_emit(
        "function headTail<T>(xs: T[]): [T | undefined, T[]] {\n"
        "  const [head, ...tail] = xs;\n"
        "  return [head, tail];\n"
        "}\n"
        "\n"
        "function swap(pair: [number, number]): [number, number] {\n"
        "  const [a, b] = pair;\n"
        "  return [b, a];\n"
        "}\n"
        "\n"
        "const [x, y, z] = [1, 2, 3];\n",
        "function headTail(xs) {\n"
        "    const [head, ...tail] = xs;\n"
        "    return [head, tail];\n"
        "}\n"
        "function swap(pair) {\n"
        "    const [a, b] = pair;\n"
        "    return [b, a];\n"
        "}\n"
        "const [x, y, z] = [1, 2, 3];\n",
        "array binding patterns (BindingElement)",
        &failed
    );

    /*
     * Class heritage: emitter.ts emitClassDeclarationOrExpression (~3557) +
     * emitHeritageClause (~4029). Selfhost-derived 80_export_class_extends.ts.
     */
    expect_emit(
        "export class Animal {}\n"
        "export class Dog extends Animal {}\n",
        "export class Animal {\n"
        "}\n"
        "export class Dog extends Animal {\n"
        "}\n",
        "class extends heritage clause",
        &failed
    );

    /*
     * export default function: parser.ts parseExportAssignment (~8736) +
     * emitter.ts emitExportAssignment (~3734). Selfhost-derived 81_export_default.ts.
     */
    expect_emit(
        "export default function main(): number {\n"
        "  return 42;\n"
        "}\n"
        "\n"
        "export const VERSION: string = \"1.0.0\";\n",
        "export default function main() {\n"
        "    return 42;\n"
        "}\n"
        "export const VERSION = \"1.0.0\";\n",
        "export default function declaration",
        &failed
    );

    /*
     * parser.ts parseClassElement (~8068) + emitter.ts emitAccessorDeclaration
     * (~2291) / emitMethodDeclaration (~2270). Selfhost-derived
     * 83_class_getter_setter.ts.
     */
    expect_emit(
        "export class Celsius {\n"
        "  private _value: number = 0;\n"
        "\n"
        "  get value(): number {\n"
        "    return this._value;\n"
        "  }\n"
        "\n"
        "  set value(v: number) {\n"
        "    this._value = v;\n"
        "  }\n"
        "\n"
        "  get fahrenheit(): number {\n"
        "    return this._value * 9 / 5 + 32;\n"
        "  }\n"
        "\n"
        "  static fromFahrenheit(f: number): Celsius {\n"
        "    const c = new Celsius();\n"
        "    c.value = (f - 32) * 5 / 9;\n"
        "    return c;\n"
        "  }\n"
        "}\n",
        "export class Celsius {\n"
        "    constructor() {\n"
        "        this._value = 0;\n"
        "    }\n"
        "    get value() {\n"
        "        return this._value;\n"
        "    }\n"
        "    set value(v) {\n"
        "        this._value = v;\n"
        "    }\n"
        "    get fahrenheit() {\n"
        "        return this._value * 9 / 5 + 32;\n"
        "    }\n"
        "    static fromFahrenheit(f) {\n"
        "        const c = new Celsius();\n"
        "        c.value = (f - 32) * 5 / 9;\n"
        "        return c;\n"
        "    }\n"
        "}\n",
        "class getters/setters + static method (83_class_getter_setter)",
        &failed
    );

    /*
     * parser.ts parseParameterWorker (~4083) parseInitializer + emitter.ts
     * emitParameter (~2221) / emitMethodDeclaration (~2270). Selfhost-derived
     * 85_class_default_params.ts.
     */
    expect_emit(
        "export class Counter {\n"
        "  private n: number = 0;\n"
        "\n"
        "  bump(by: number = 1): void {\n"
        "    this.n += by;\n"
        "  }\n"
        "\n"
        "  value(): number {\n"
        "    return this.n;\n"
        "  }\n"
        "\n"
        "  reset(to: number = 0, quiet: boolean = false): void {\n"
        "    this.n = to;\n"
        "    if (!quiet) {\n"
        "      this.bump(0);\n"
        "    }\n"
        "  }\n"
        "}\n"
        "\n"
        "export function logLines(prefix: string = \"info\", ...lines: string[]): string {\n"
        "  return lines.map((l) => \"[\" + prefix + \"] \" + l).join(\"\\n\");\n"
        "}\n",
        "export class Counter {\n"
        "    constructor() {\n"
        "        this.n = 0;\n"
        "    }\n"
        "    bump(by = 1) {\n"
        "        this.n += by;\n"
        "    }\n"
        "    value() {\n"
        "        return this.n;\n"
        "    }\n"
        "    reset(to = 0, quiet = false) {\n"
        "        this.n = to;\n"
        "        if (!quiet) {\n"
        "            this.bump(0);\n"
        "        }\n"
        "    }\n"
        "}\n"
        "export function logLines(prefix = \"info\", ...lines) {\n"
        "    return lines.map((l) => \"[\" + prefix + \"] \" + l).join(\"\\n\");\n"
        "}\n",
        "class/function default parameters (85_class_default_params)",
        &failed
    );

    /*
     * parser.ts export + modifiers before ClassKeyword + transformers/ts.ts
     * static field lowering. Selfhost-derived 87_export_abstract.ts.
     */
    expect_emit(
        "export abstract class Shape {\n"
        "  static instances: number = 0;\n"
        "  constructor() {\n"
        "    Shape.instances++;\n"
        "  }\n"
        "  abstract area(): number;\n"
        "  describe(): string {\n"
        "    return \"area=\" + this.area();\n"
        "  }\n"
        "}\n",
        "export class Shape {\n"
        "    constructor() {\n"
        "        Shape.instances++;\n"
        "    }\n"
        "    describe() {\n"
        "        return \"area=\" + this.area();\n"
        "    }\n"
        "}\n"
        "Shape.instances = 0;\n",
        "export abstract class: static field + strip abstract (87_export_abstract)",
        &failed
    );

    /*
     * transformers/ts.ts elides TypeAliasDeclaration. Matches
     * fixtures/emitter/selfhost-derived/89_type_alias_erase.ts (transpileModule).
     */
    expect_emit(
        "export type ID = string;\n"
        "export type Predicate<T> = (x: T) => boolean;\n"
        "\n"
        "export type Shape =\n"
        "  | { kind: \"circle\"; r: number }\n"
        "  | { kind: \"square\"; side: number };\n"
        "\n"
        "type InternalHelper = number | string;\n"
        "\n"
        "export function pick<T>(xs: T[], pred: Predicate<T>): T[] {\n"
        "  const out: T[] = [];\n"
        "  for (const x of xs) {\n"
        "    if (pred(x)) out.push(x);\n"
        "  }\n"
        "  return out;\n"
        "}\n"
        "\n"
        "export function describe(s: Shape): string {\n"
        "  return s.kind === \"circle\" ? \"c:\" + s.r : \"s:\" + s.side;\n"
        "}\n",
        "export function pick(xs, pred) {\n"
        "    const out = [];\n"
        "    for (const x of xs) {\n"
        "        if (pred(x))\n"
        "            out.push(x);\n"
        "    }\n"
        "    return out;\n"
        "}\n"
        "export function describe(s) {\n"
        "    return s.kind === \"circle\" ? \"c:\" + s.r : \"s:\" + s.side;\n"
        "}\n",
        "type alias erase (89_type_alias_erase)",
        &failed
    );

    return failed;
}
