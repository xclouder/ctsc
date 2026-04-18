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

## 并行模式：按 phase 分 5 个 worker 同时跑（快 N 倍）

单 loop 完全串行（一次只处理一个 fixture）。因为不同 phase 的代码改动面
几乎不重叠（`ctsc/src/scanner/`、`ctsc/src/parser/` 等），可以按 phase 开
多个 worker 并行，每个 worker 用自己独立的 `ctsc/build/phase-<P>/` 和
`harness/state/progress.<P>.json`，互不踩脚。

```powershell
# 默认两档：composer-2 先刷，卡住 2 次自动升级到 Opus-4.7，省 token。
.\scripts\run-loops.ps1

# 只攻 scanner + parser 积压
.\scripts\run-loops.ps1 -Phases scanner,parser

# 纯 composer-2、不升级（极限省 token）
.\scripts\run-loops.ps1 -FallbackModel ''

# 老行为：Opus 直接上，不做两档
.\scripts\run-loops.ps1 -Model claude-opus-4-7-xhigh -FallbackModel ''

# 重试之前 defer 掉的
.\scripts\run-loops.ps1 -RetryDeferred
```

**两档模型机制**：每个 fixture 若连续 `-FallbackAfter`（默认 2）次 no-progress，
这个 fixture 会自动切到 `-FallbackModel`（默认 `claude-opus-4-7-xhigh`）
继续尝试直到 watchdog 5 次无进展才 defer。简单规则：cheap 模型刷日常，
Opus 接盘硬骨头。日志里会出现 `[escalate] model -> …`。

并行模式特性：
- 每个 worker 一个独立 ninja build tree（`ctsc/build/phase-scanner/` …），
  cmake 互不干扰。
- 每个 worker 一个独立 progress 文件（`progress.scanner.json` …），避免并
  发写覆盖。
- 所有 PID 记在 `harness/reports/loops.pids`，同时自动生成一键 `loops.stop.ps1`。
- 实时尾随单 phase 日志：
  `Get-Content -Wait -Tail 40 'harness\reports\loop-<stamp>-<phase>.log'`

聚合查看所有 phase 的总进度：

```powershell
cd harness
npx tsx src/cli.ts status --all
```

⚠ 注意：
- 单 loop 与并行 loop **不要同时跑**，因为它们可能同时 edit `ctsc/src/*`
  的共享头文件（agent 之间没有 merge 协议）。开并行前先
  `taskkill /PID (Get-Content harness\reports\loop.pid) /T /F` 停掉旧的。
- Cursor 后端对同账号并发调用有速率限制。worker 数建议 ≤ 5，再多可能互相排队。

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
- `npm run selfhost`                       — M1 冒烟测试：把
  `harness/selfhost/packages/` 下的每个 mini-package 用 ctsc 转译，
  与 `ts.transpileModule` 做 byte-for-byte diff，并用 node 跑
  `runtime.mjs` 验证真的能执行。新包只要放一个 `src/*.ts` 就会被
  自动发现。

## Self-host 反馈闭环

self-host 的价值是暴露 fixture 覆盖不到的**复合 bug**（一个 .ts 文件里
同时用 export / class / switch / 泛型 / 字符串转义……）。流程：

1. `npm run selfhost` 跑出一份不匹配报告。
2. 每个独立 bug 抽成一个最小 fixture，放到
   `fixtures/emitter/selfhost-derived/NN_<feature>.ts`。planner 会自动
   发现并把它们纳入 curriculum。
3. 跑 `npm run loop`（或并行脚本）让 agent 修到过。
4. 再跑一次 `npm run selfhost`，通常会有新 bug 浮上来 —— 回到第 2 步。

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
