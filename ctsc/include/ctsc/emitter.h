#ifndef CTSC_EMITTER_H
#define CTSC_EMITTER_H

#include "common.h"
#include "ast.h"
#include "buffer.h"
#include "utf8.h"

/*
 * JavaScript 打印器。目前覆盖 tsc transpileModule ES2020 的最小子集：
 *   - SourceFile / ExpressionStatement / EmptyStatement
 *   - Identifier / NumericLiteral / StringLiteral
 *   - VariableStatement (var/let/const) + VariableDeclaration
 *   - FunctionDeclaration + Block + ReturnStatement
 *   - BinaryExpression / CallExpression
 *
 * tsc 的 emitter 行为参考：upstream/TypeScript/src/compiler/emitter.ts。
 * 目标：对我们已有的 fixture 逐字节匹配 ts.transpileModule 的输出。
 */

/*
 * Emit the JS form of `sourceFile` into `out`.
 *
 * `source` is the original UTF-16 source buffer used by the scanner; the
 * emitter uses it to replay leading comment trivia for source files whose
 * statements list is empty (mirrors emitter.ts emitBodyWithDetachedComments
 * + emitLeadingComments at detachedRange.end=0 when statements.length===0).
 * Pass NULL when the source is unavailable; comment replay is then skipped.
 */
void ctsc_emit_js(const CtscNode* sourceFile, const CtscUtf16Buf* source, CtscBuffer* out);

#endif
