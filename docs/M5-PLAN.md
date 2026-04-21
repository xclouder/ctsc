# M5：能编译真实小包

目标：`ctsc compile demo/real.ts` 能对使用 `Array.prototype.map`、
`console.log`、`Object.keys` 之类最常见 API 的代码，产出与 `tsc 5.6.3` 字节
一致的 JavaScript（target 先锚在 ES2020）。

M5 完成 ≈ checker 200-300 fixture、emitter 补齐 ES2020 → ES5 解糖、module
系统最小可用。

## 设计原则

1. **先类型层、再模块层、再 emit 层**。类型系统是 lib.d.ts 的前置依赖，
   模块系统是多文件 lib 的前置依赖。
2. **lib.d.ts 以"部分扫描"方式接入**：不一次性吃下 62k 行，而是每个
   sub-milestone 接入当前能消化的最小子集（例如 M5.6 先只要 `Array<T>`
   的几个方法），其余暂时放到 `lib.ctsc.unsupported.d.ts`。
3. **每个 sub-milestone 结束后 tag 一次**：`m5.0-declare`、
   `m5.1-overload`……
4. **分阶段 oracle 演进**：保留现有 `diag` / `types` 通道；新增
   `ts-compile` 通道（M5.4 起），该通道比较 `tsc --noEmitOnError=false
   <files>` 产出的 `.js` 文件与 ctsc 的 `.js`。

## 里程碑详表

### M5.0 `declare` 与 ambient 声明

**为什么先做**：lib.d.ts 里的所有符号都是 `declare`，没有这个就无法注入
内建类型。

wave 计划：

- wave 13 · `declare` 变量/函数基础
  - `declare const x: number;`
  - `declare let y: string;`
  - `declare function f(): number;`
  - `declare function g(x: number): string;`
  - 参考 `upstream/TypeScript/src/compiler/parser.ts` 的 `parseDeclaration`
    分支、`binder.ts` 中 `SymbolFlags.Ambient`。

- wave 14 · `declare class` / `declare interface` / 接口合并
  - `declare class Foo { x: number; }`
  - `declare interface Point { readonly x: number; }`
  - `interface P { x: number; } interface P { y: string; }` 同文件合并。
  - **延后**：`declare namespace N { ... }` 的成员查找需要 parser/binder/
    checker 协同改造，挪到 M5.0.5（单独 sub-wave）。

- wave 15 · `declare namespace` + 泛型接口合并（M5.0.5 / 合并入 wave 15）
  - `declare namespace N { const x: number; function f(): string; }`
    → `N.x` / `N.f()` 跨成员查找（parser 吃掉 `declare` 前缀，binder
    把 ModuleBlock 成员登记到 N 的 locals，checker 的
    PropertyAccessExpression 看 ModuleDeclaration 符号时走 locals 查询）；
  - `interface Array<T> { foo(): T; }` + `interface Array<T> { bar():
    number; }` 合并成员 —— 前半同名合并已在 wave 14 搞定，这里验证带
    泛型的合并；
  - 参考 `checker.ts` 中 `mergeSymbol` / `getDeclaredTypeOfClassOrInterface`
    / `bindModuleDeclaration`。

**退出标准**：至少 15 个 fixture，覆盖 declare 变量/函数/类/接口合并/命名
空间；checker 继续保持 100%。

### M5.1 函数重载 + 泛型约束 + 默认类型参数

**为什么**：lib.d.ts 的 `Array.prototype.map` 就是重载 + 约束 + 默认。

wave 计划：

- wave 16 · overload signatures（函数）
  - `function f(x: number): string; function f(x: string): number;
    function f(x: any): any { ... }`
  - 参考 `checker.ts` 中 `resolveCall` / `chooseOverload`。

- wave 17 · overload signatures（方法 / 构造函数）
  - `class C { m(x: number): string; m(x: string): number; m(x: any):
    any { ... } }`

- wave 18 · 泛型约束 `<T extends U>`
  - `function len<T extends { length: number }>(x: T): number { return
    x.length; }`
  - `class Box<T extends string> { ... }`

- wave 19 · 默认类型参数 `<T = string>`
  - `function id<T = number>(x: T): T { return x; }`

**退出标准**：至少 12 个 fixture；`chooseOverload` 能选中最具体签名；
未匹配重载报 TS2769。

### M5.2 rest / spread / tuple 进阶

wave 计划：

- wave 20 · rest parameter `...args: T[]`
- wave 21 · spread 调用 `f(...a)`（参数展开）
- wave 22 · tuple 类型进阶：可选 `[number, string?]`、rest
  `[number, ...string[]]`

**退出标准**：至少 9 个 fixture；满足重载用到的 `...args: any[]` 签名
（lib.d.ts 里非常普遍）。

### M5.3 keyof / 索引访问 / 简单映射类型

wave 计划：

- wave 23 · `typeof X`（值转类型查询）：`const x = { a: 1 }; type X =
  typeof x;`
- wave 24 · `keyof T`（含 `keyof any` / 联合字面量）
- wave 25 · 索引访问 `T[K]`（含条件键）
- wave 26 · 映射类型 homomorphic：`Partial<T>`、`Readonly<T>`
- wave 27 · 映射类型 `Pick<T, K>`、`Record<K, V>`

**退出标准**：至少 18 个 fixture；`Partial<{x:number}>` 等价于
`{x?: number}`。

### M5.4 模块语法解析（import / export）

wave 计划：

- wave 28 · `export const x = 1;`、`export function f(){}`、`export
  default 1;`
- wave 29 · `import { x } from "./m";`、`import d from "./m";`、
  `import * as m from "./m";`
- wave 30 · re-export：`export { x } from "./m";`、`export * from "./m";`
- wave 31 · `import type` / `export type`

**退出标准**：解析通过 + 符号表里正确标记 import/export 标志；**暂不跨
文件解析**。

### M5.5 多文件编译 + 最小模块解析

wave 计划：

- wave 32 · ctsc 新增 `--project`（或 `-p`）和 `--outDir`，harness 新增
  `ts-compile` oracle channel；
- wave 33 · 相对路径解析（只认 `./foo.ts` / `./foo/index.ts`）；
- wave 34 · 跨文件符号表合并 + 交叉引用；
- wave 35 · `node_modules/@types` 一级解析（暂只 `@types/node/index.d.ts`
  的选取）。

**退出标准**：`ctsc -p demo/tsconfig.json` 能把 2-3 个文件的工程编译出一
堆 `.js` 文件，与 `tsc -p` 字节级一致。

### M5.6 最小 lib.ctsc.d.ts + 内建方法

**为什么重要**：这是 M5 的真正 payoff。

wave 计划：

- wave 36 · 注入 lib 的管道（运行 `ctsc` 时隐式把 `lib.ctsc.d.ts` 放到
  program 的第一个文件）
- wave 37 · `lib.ctsc.core.d.ts`：`Object`、`Function`、`String`、
  `Number`、`Boolean`、`Symbol`、`BigInt` 的最小成员
- wave 38 · `Array<T>`：`length` / `push` / `pop` / `map` / `filter` /
  `reduce` / `forEach`
- wave 39 · `console.log / error / warn`、`JSON.stringify / parse`
- wave 40 · `Promise<T>` 的最小签名（`then` / `catch` / `finally`）
  —— 足够编译 async/await 但不做真运行时
- wave 41 · `Map<K,V>` / `Set<T>`

**退出标准**：`[1,2,3].map(x => x*2)` 类型正确，emit 与 tsc 一致。

### M5.7 ES2020 → ES5 target 下降 emit

**为什么**：很多真实工程 target 到 ES5。这一节只做"最常见的 10 个解糖"。

wave 计划：

- wave 42 · `class` → 构造函数 + prototype
- wave 43 · `async/await` → generator helper（`__awaiter` / `__generator`
  走 tslib）
- wave 44 · `?.` optional chaining 解糖
- wave 45 · `??` nullish coalescing 解糖
- wave 46 · `for...of` → for 循环 + iterator
- wave 47 · `**` 幂运算 → `Math.pow`
- wave 48 · 默认参数 `(x = 1)` → 函数体内 `if (x === undefined)`
- wave 49 · rest / spread 在参数/数组/对象中的解糖

**退出标准**：target=ES5 emit 与 tsc 字节一致；tslib helper 引用方式一致。

## 预估

| 子里程碑 | wave 数 | agent 挂机估算 |
|---|---|---|
| M5.0 | 3 | ~3 天 |
| M5.1 | 4 | ~4 天 |
| M5.2 | 3 | ~3 天 |
| M5.3 | 5 | ~6 天 |
| M5.4 | 4 | ~4 天 |
| M5.5 | 4 | ~6 天 |
| M5.6 | 6 | ~8 天 |
| M5.7 | 8 | ~10 天 |
| **合计** | **37 wave** | **~45 天挂机（含调试）** |

这是一个理想值，实际因为复杂特性（条件类型、映射类型、overload
resolution）可能 2×，按 **2-3 个月挂机** 规划。

## 下一步

**立即开工 wave 13**：`declare const` / `declare let` / `declare function`
基础。fixture 设计：

- `declare/01_declare_const.ts`
- `declare/02_declare_let.ts`
- `declare/03_declare_function.ts`
- `declare/04_declare_function_with_params.ts`
- `declare/05_declare_function_overload_stub.ts`（两个签名，先确保第一个
  签名参与类型推断即可，完整 overload 在 M5.1）
- `declare/06_declare_ambient_access.ts`（在 ambient 声明后直接使用该符号）
