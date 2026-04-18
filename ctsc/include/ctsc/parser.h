#ifndef CTSC_PARSER_H
#define CTSC_PARSER_H

#include "common.h"
#include "ast.h"
#include "diagnostic.h"
#include "buffer.h"

struct CtscArena;

typedef struct {
    struct CtscArena*   arena;
    CtscDiagnosticList* diagnostics;
    CtscNode*           sourceFile;
} CtscParseResult;

/*
 * 解析源码产出 SourceFile 节点。当前实现极简：只识别空文件、分号空语句、
 * 以及孤立的 Identifier/NumericLiteral/StringLiteral 表达式语句。
 * 其他情况会把未识别的 token 跳过并产生 diagnostic；harness 将驱动
 * agent 逐步扩展覆盖。
 *
 * 参考：upstream/TypeScript/src/compiler/parser.ts :: createSourceFile()
 */
CtscParseResult ctsc_parse(const char* src, size_t len, struct CtscArena* arena);

/* 把 SourceFile 序列化成与 oracle 对齐的 JSON。 */
void ctsc_ast_dump_json(const CtscNode* sourceFile, CtscBuffer* out, bool pretty);

#endif
