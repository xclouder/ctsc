# ctsc — Agent 指南

本仓库是一个长期运行的实验：把 Microsoft 的 TypeScript 编译器（`tsc`）移植
到纯 C 语言，由一套 agent harness 驱动 —— 它把 ctsc 的输出与 `tsc` 的输出
做 diff，并持续迭代直到完全对齐。

如果你是一个正在编辑本仓库的 AI agent，请严格遵守以下规则。每次你被调用
之后，harness 都会重新构建并重新跑测。

## 不可协商的规则

1. **只修改 `ctsc/` 下的文件**。harness（`harness/`）、Cursor 规则
   （`.cursor/`）以及冻结的参考代码（`upstream/`）禁止修改，除非用户明确
   让你改。
2. **与 `tsc` 做到逐字节一致**。我们逐字照搬 `ts.SyntaxKind` 名称、UTF-16
   位置以及诊断信息文本。不要"改进" tsc 的行为。
3. **先读参考实现**。编辑任何 phase 之前，先打开
   `upstream/TypeScript/src/compiler/` 下对应的文件，并在 commit / PR
   描述里引用你参考的行号。
4. **diff 尽量小**。优先扩展已有模块；不要没理由地新增顶层目录。
5. **每次修改都要 build + 跑测**。`cmake --build ctsc/build/default` 必须
   通过，`ctsc/build/default/ctsc_tests` 必须 pass。若有测试过期，更新它；
   **不要删除测试**。
6. **新行为要写新测试**。每新增一个 scanner token / AST 节点 / checker
   规则，都要在 `ctsc/tests/` 下加单测。
7. **保持 UTF-16 偏移量**。tsc 以 UTF-16 code unit 报告位置；
   `ctsc_utf16_from_utf8` 已经完成转换，请保持 `scanner.pos` 为 UTF-16
   索引。

## 架构速览

- `ctsc/include/ctsc/` — 公共头文件。新增头文件放这里（例如 `parser.h`、
  `ast.h`）。
- `ctsc/src/core/` — 分配器、字符串切片、哈希表、诊断列表、UTF-8、
  JSON writer、文件 IO。这些模块保持零依赖。
- `ctsc/src/scanner/` — 扫描器与 token 名表。
- `ctsc/src/parser/`、`binder/`、`checker/`、`transformer/`、`emitter/`、
  `driver/` — 随着 curriculum 解锁逐步加入。

## Harness 契约

你收到的 prompt 会包含：
- 失败 fixture 的源码；
- tsc 期望的 JSON 输出；
- ctsc 的实际 JSON 输出；
- 一行 diff 摘要。

你的任务：修改 `ctsc/` 使得同一个 fixture 下次运行时与 tsc 对齐。harness
会重跑**所有**之前已经绿的 fixture，**不要让它们回归**。

## 当某个 TS / JS 特性无法干净地映射到 C 时

- 把决策记在 `docs/semantics-notes.md`（没有就新建）。
- **优先保真，而不是图省事**。例：JS 字符串本质是 UTF-16 code unit 序列，
  **不要**偷偷"优化"成 UTF-8 字节偏移。

## Commit message

使用祈使句。引用你参考的 TypeScript 源码位置，例如：

```
scanner: handle BigInt literal suffix (n)

Mirrors upstream/TypeScript/src/compiler/scanner.ts:~3070 (scanBigIntLiteral).
```
