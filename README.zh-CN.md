# ctsc — 由 harness 驱动的 tsc C 语言移植

一个长期运行的实验：用纯 C 语言移植 Microsoft 的 TypeScript 编译器
（`tsc`）。做法是让一个 Cursor agent 在循环里跑 —— 把 ctsc 的输出与 `tsc`
的输出做 diff，反复迭代直到完全对齐。

## 目录结构

- `upstream/TypeScript/` — `microsoft/TypeScript` 的浅克隆（作为 oracle）。
- `ctsc/` — C 语言移植本体（CMake + Ninja）。
- `harness/` — 自迭代 harness（Node + TypeScript）。
- `fixtures/` — curriculum 测试用例，按 phase / stage 分组，文件名以难度
  前缀开头（`01_...`）。
- `scripts/bootstrap.ps1` — 一键初始化脚本（拉取 upstream、装 harness
  依赖、检测 C 工具链）。
- `scripts/run-loop.ps1` — 一键在后台启动长跑 loop（见下）。
- `AGENTS.md` / `AGENTS.zh-CN.md` — 所有修改 `ctsc/` 的 agent 必须遵守的
  规则。

## 快速开始

```powershell
# 1. 一次性初始化
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1

# 2. 构建 ctsc （需要 MSVC Developer PowerShell，或 PATH 里有 LLVM/clang）
cmake --preset default -S ctsc
cmake --build ctsc/build/default

# 3. 不调 agent、只跑一次流水线冒烟
cd harness
npm run loop -- --dry --phase scanner --max 5

# 4. 真正驱动 agent loop（需要先装并登录 Cursor CLI）
#    安装：irm 'https://cursor.com/install?win32=true' | iex
#    登录：agent login   （或设 $env:CURSOR_API_KEY）
npm run loop -- --phase scanner --max 20 --build
```

## 挂后台长跑（推荐）

用仓库根目录下的脚本，一条命令启动、落日志、返回 PID：

```powershell
cd J:\ai-ideas\typescript-speedup-cursor
.\scripts\run-loop.ps1 -RetryDeferred -Model claude-opus-4-7-xhigh -StatusEvery 20
```

它会：
- 在后台启动 `npm run loop -- --forever --build --retry-deferred --model ...`；
- 把 stdout / stderr 写到 `harness/reports/loop-<时间戳>.log`；
- 在 `harness/reports/loop.pid` 写入 PID，便于停机。

监控：

```powershell
# 实时尾随日志
Get-Content -Wait -Tail 40 'harness\reports\loop-latest.log'

# 汇总进度
cd harness; npm run status
```

干净停机（会递归 kill tsx / agent 所有子孙进程）：

```powershell
taskkill /PID (Get-Content harness\reports\loop.pid) /T /F
```

## Harness 常用命令

- `npm run list`                           — 按 curriculum 顺序列出所有 fixture
- `npm run status`                         — 汇总 pass / fail / deferred 状态
- `npm run oracle -- <fixture-id>`         — 打印 tsc oracle 输出
- `npm run diff   -- <fixture-id>`         — 展示 ctsc 与 tsc 的 diff
- `npm run loop   -- --dry`                — 不调 agent 冒烟测试整条流水线
- `npm run loop   -- --no-agent`           — 跑真实 ctsc 但跳过 agent（把自然
  通过的 fixture 收掉，失败的暂存到 deferred）
- `npm run loop   -- --forever --build`    — 无限循环 + 自动重建
- `npm run loop   -- --retry-deferred`     — 启动时把之前 defer 的 fixture
  拎回来重新挑战
- `npm run loop   -- --model <name>`       — 指定 agent 模型（如
  `composer-2`、`gpt-5.3-codex-high`、`claude-opus-4-7-xhigh`）

## Phase 路线图

- Phase 0：core（arena / UTF-8 / UTF-16 / hashmap / diagnostic / JSON writer）— **已完成**。
- Phase 1：scanner。MVP 已实现；30 + 200 条 fixture；harness 对 token stream 做 diff。
- Phase 2：parser。AST JSON differ 与 `--dump-ast`。
- Phase 3：binder。
- Phase 4：checker。最庞大的 phase；分子阶段推进：basic → structural →
  generics → inference → conditional / mapped → control-flow narrowing。
- Phase 5：transformer + emitter（`.js` / `.d.ts` 逐字节一致）。
- Phase 6：CLI / tsconfig / project references。

## 关于"逐字节兼容"

`tsc` 有数万条 baseline 测试。完全对齐是一个**以年为单位**的目标，不是一个
session 就能交付的东西。harness 的设计目标就是**可无限期持续运行**、
把进度 checkpoint 到 `state/progress.json`、让 agent 一个一个啃掉失败用例。
agent 必须遵守的规则详见 [`AGENTS.md`](AGENTS.md) / [`AGENTS.zh-CN.md`](AGENTS.zh-CN.md)。
