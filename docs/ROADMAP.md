# ctsc 路线图：从 M4 到完整 tsc

最后更新：2026-04-21（checker 125/125，M4.3 完成 wave 12）。

本文档是"从现在走到 tsc 5.6.3 完全对齐"的总规划。每一节都链接到更细的 wave
计划文档。这里的估算都是 **agent loop 实际挂机时长**（composer-2 为主、
claude-opus 兜底），不是人类工程师时长。

## 全景

| Milestone | 终点口号 | 主要能力 | 预计 agent 挂机 |
|---|---|---|---|
| M4（已完成） | checker 骨架 | 61→125 fixture，几乎每个 TS 语言特性都有"一个代表" | — |
| **M5** | **"能编译一个真实的小包"** | 模块系统、多文件、lib.d.ts 子集、重载、keyof/索引访问 | 2-4 个月 |
| M6 | "工业可替代" | .d.ts 生成、装饰器、JSX、完整 CFA、条件/映射类型、source map、完整 lib.d.ts | 6-12 个月 |
| M7 | "完整对齐 tsc 5.6.3 基线" | tsc `tests/baselines/reference` 逐文件字节级通过 | 12-24 个月 |

## 通用工作方式

1. 每个 milestone 拆成若干 sub-milestone（M5.0/M5.1…），再拆成 wave；
2. 每个 wave 5-12 个 fixture，`npm run loop` 跑完就归档 + tag；
3. harness、oracle 通用于所有阶段，新能力优先用已有 oracle channel（`types` /
   `diag` / 其他）；
4. 不重复造 fixture —— 新特性如果能复用之前的 fixture 目录（例如
   `generics/`）就放进同一个目录，保持分类直觉；
5. `AGENTS.zh-CN.md` 的规则永远优先：
   - 先读 `upstream/TypeScript/src/compiler/` 再动手；
   - UTF-16 偏移不变；
   - 绿的 fixture 不回归。

## 里程碑概览

### M5：能编译真实小包（详细见 [`M5-PLAN.md`](M5-PLAN.md)）

子里程碑：

| ID | 名字 | 作用 | wave 预估 |
|---|---|---|---|
| M5.0 | `declare` + ambient | 为加载 lib.d.ts 打基础 | 3-5 |
| M5.1 | 函数重载 + 泛型约束 + 默认类型参数 | lib.d.ts 里每个 API 都要 | 4-6 |
| M5.2 | rest parameter + spread + tuple 进阶 | 很多 lib API 用 `...args: T[]` | 3-4 |
| M5.3 | keyof / 索引访问 / 简单映射类型 | `Pick/Omit/Record/Partial` | 5-7 |
| M5.4 | 模块语法：`import`/`export` 解析 | 只解析、不跨文件 | 3-4 |
| M5.5 | 多文件编译 + 简易模块解析 | `ts.Program` 级 | 5-7 |
| M5.6 | 最小 lib.ctsc.d.ts 注入 + 内建方法 | `Array.prototype.map/filter/reduce`、`console.log`、`Object.keys` | 6-10 |
| M5.7 | target 下降 emit（ES2020 → ES5 子集） | class/async/??/?. 解糖 | 6-10 |

**产出**：`ctsc compile demo/real.ts` 对 `[1,2,3].map(x => x*2)` 一类代码和
tsc 字节一致（emit 至少能跑起来）。

### M6：工业可替代

子里程碑：

| ID | 主题 |
|---|---|
| M6.0 | 完整控制流分析（循环、try/catch、switch 穷尽、definite assignment） |
| M6.1 | instanceof / `in` / 自定义类型守卫 / `asserts x is T` |
| M6.2 | 条件类型 + `infer`、映射类型完整、模板字面量类型 |
| M6.3 | .d.ts 声明文件生成 |
| M6.4 | JSX（React / Preact / jsxFactory / jsxImportSource） |
| M6.5 | Decorator（legacy + Stage 3） |
| M6.6 | Source map v3 |
| M6.7 | 模块解析矩阵（node / node16 / nodenext / bundler、paths、baseUrl） |
| M6.8 | 完整 lib.d.ts（DOM、WebWorker、ES2015-ES2024） |
| M6.9 | Pretty diagnostics + error recovery + watch mode |

**产出**：`ctsc` 可以替代 `tsc` 跑 `vite` / `tsup` 一类的小型工程。

### M7：完整对齐 tsc 5.6.3 基线

- 批量导入 `tsc` 的 baseline fixture（~60k 文件）；
- 字节级 diff；
- 角落用例（ES3 target、legacy decorator、UMD、AMD）；
- Language Service API；
- Project references / incremental / `tsc --build`。

## 红线（任何时候都不做）

- 不去"改进" tsc 的行为，只做镜像；
- 不引入非 C 依赖（除了现有的 libc 子集）；
- 不为了短期速度牺牲 UTF-16 一致性；
- 不删测试来让构建通过。

## 决策日志

- 2026-04-21：完成 M4.3 wave 12，checker 125/125，打 tag
  `m4.3-125-checker`。决定把 M5 定为"能编译真实小包"，优先级顺序是
  declare → overload → rest/spread → keyof → modules → lib → target
  下降。
