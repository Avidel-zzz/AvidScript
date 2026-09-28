# 可组合语言能力合同：P66.C 到跨语言模块

状态：P66.C 设计增量，2026-09-28。本文规定下一段实现与验收，不改变已完成的 P66.A/B、既有 Phase 状态或已发布的 IR 1–34 产物。目标版本暂定 C# Semantic 54/1.63、Guest IR 35/1.34；实施前若编号已分配，必须另取新版本，不能重解释旧字节。

首个实现切片已在 Guest IR 中加入可选清单字段、精确执行基线/能力 ID 校验及 IR 35 严格 JSON 读取。旧版本不能携带新清单；IR 35 即使清单结构有效，仍由现有执行版本准入拒绝，直到 Semantic、WASM emitter 和原生读取者同步接入。固定 SDK 的 Guest IR 398/398、WASM Backend 415/415、C# Guest 4490/4490 通过。这是格式与负例的进度，不是静态初始化 + token 已可执行。

## 现有阻塞

[SemanticAnalyzer](../../Tools/AvidScript.CSharpSemantic/Analysis/SemanticAnalyzer.cs)在启用 async catch 变量时拒绝静态初始化；标准 token 又依赖 catch 变量。[GuestCancellationTokenValidator](../../Tools/AvidScript.GuestIr/Validation/GuestCancellationTokenValidator.cs)拒绝 IR 34 中的 `StaticStorage`。原始 [29 个成员 await 场景](../Phase66/P66.C10_Await_Member_Assignment_Contract.md#同源基准矩阵)需要静态对象初始化，当前通过的是 Semantic 51 → IR 31；标准 token 专项使用 Semantic 53 → IR 34。两组通过不能证明同一个业务模块能同时使用两种能力。

当前每个新组合靠一个外层版本和 `BaseProfile` 视图回退到旧执行版本。[GuestValidationContext](../../Tools/AvidScript.GuestIr/Validation/GuestValidationContext.cs)与 [GuestStaticStorage](../../Tools/AvidScript.GuestIr/Model/GuestStaticStorage.cs)已有多重视图；[WASM provenance](../../Tools/AvidScript.WasmBackend/Codegen/WasmModuleCompiler.cs)和[原生读取者](../../Source/AvidScriptRuntime/Private/Diagnostics/AvidScriptLanguageErrorCatalog.cpp)只编码单个外层版本与 base。继续给每种组合套一个版本，会让语言前端、缓存和 Host 的组合数量随能力增长。

## 合同边界

| 层 | 负责什么 | 不负责什么 |
| --- | --- | --- |
| 语言前端 | C# 用 Roslyn 证明源码语义、生成确定性的功能计划；后续语言有自己的前端版本 | 不把 C# 的异常对象或堆布局当公共 ABI |
| Guest IR | 规定执行指令集、能力集合、各能力的计划与依赖；一个模块只验证一次规范化结果 | 不信任前端声明就放行缺失的计划或 import |
| WASM/Host | 将已验证能力写入产物与 provenance；Runtime 按产物版本和授权导入执行 | 不从 WASM 名称猜能力，也不允许未知导入试探 |
| 跨语言模块接口 | 另行定义数据、资源、调用、异步和错误的稳定交换合同 | 不直接共享各语言的 Task、异常对象、GC 根或闭包内存 |

IR 35 将 `execution_base` 与**排序、去重、版本化**的 `capabilities` 列表分开。首版显式允许经验证的 14/1.13、17/1.16 与 29/1.28 基线；本次静态初始化 + async + token 组合使用 29/1.28，纯计算或同步模块不被迫携带 async 基线。基础指令集与能力版本是两条轴：后续增加一个可独立验证的能力，不再为所有已有能力的排列新增外层 IR。现有计划结构可以在首轮复用，但其 `BaseSchemaVersion/BaseIrVersion` 必须全部等于所选执行基线；新合同不允许多个互相矛盾的 base。规范化后只用真实输入模块做授权与所有权验证，不通过改写版本字段的临时视图绕开新组合。

`execution_base` 自身已有任务、错误、所有权和指令合同；能力列表只列可独立组合的增量，不把 IR 29 固有计划重新声明一遍。新验证器最终仍须检查基线与增量的全部计划、指令和 import，不能因为一项未列在增量清单中就跳过基线校验。

首轮已知能力及依赖：

| 能力 ID | 必须存在的计划或类型 | 依赖 |
| --- | --- | --- |
| `managed.static_storage@1` | 静态槽、初始化方法与 owner | 托管堆、执行域与包代次；单个 Session 退出不释放共享静态状态 |
| `async.await_readiness@1` | await guard、回调/取消出口 | IR 29 异步执行基线 |
| `async.cancellation_identity@1` | 类型化取消 writer 与 source 身份 | 异步任务、有效 Session |
| `error.exception_values@1` | 异常对象类型、根、读取路径 | 语言错误 outcome；首轮仅开放已验证的 29/1.28 异步基线 |
| `error.cancellation_token_value@1` | 8 字节 token 值布局；若读异常 token，则需 reader import | token 值本身可用于同步代码；异常 reader 另需异常根 |

这组 ID 只声明对应计划存在，**不能**代替计划验证。一个缺失、重复、乱序、未知版本、未声明却出现的计划，或声明了但没有所需类型/import 的产物都应拒绝。能力依赖是有向无环图，验证时显式检查，不以“更高的 Semantic/IR 编号”推断所有低版本能力。相同源码、能力集合、生成绑定包和工具链输入必须得到相同规范 IR 字节与 WASM hash。

Semantic 54 的 C# 文档记录已实际投射的语言能力与源码 hash；IR 35 记录执行能力与对应 Semantic hash。第二语言可产出同一 IR 能力，却必须保留自己的前端种类与版本，不能伪装成 C# Semantic 54。默认 CLI、缓存、generated facade、Editor Build And Bind 与包发布读取同一份能力判定；禁止一个入口接受组合而另一个入口默默退回旧版。

WASM 的新 provenance 同时记录 `guest_ir=35/1.34`、`execution_base=29/1.28`、规范能力列表及前端合同。原生读取者对 IR 35 使用严格新格式，对 IR 1–34 保持原有精确解析；不能把新字段塞进旧 IR，也不能把旧版 reader 当成新版本。Host 验证实际加载的 WASM 与包中已审核的 hash、provenance 和 import 表一致，再按 Session、World、线程、根与代际检查执行。未知必需能力、未来版本、错误签名、别名 import 和能力/计划/导入不一致均 fail closed。

## 落地顺序与验收

| 步骤 | 改动范围 | 可停止的验收结果 |
| --- | --- | --- |
| 1. 规范化模型 | Semantic/IR 的能力列表、依赖图、确定性序列化与验证器；旧版只读兼容 | IR 1–34 的固定 fixture 字节/行为不变；同集合乱序生成同字节；删字段、伪造旧版、未知能力和冲突 base 全被拒绝 |
| 2. 同一源码组合 | C# 分析、lowering、WASM emitter；静态槽、await、异常根与 token reader 同模块 | [AwaitMemberAssignment.cs](../../Fixtures/Phase66/AwaitMemberAssignment.cs)业务字节及 SHA-256 `265a7e71e2c744681d099ee6b11a93fcf94b151980205cddf155c46f602249d9` 不变；原始 29 场景逐项对照 .NET 的值、Trace、Task 终态和每轮快照。测试适配层可追加 token 观察，不能改业务分支或预期 |
| 3. 原生执行 | IR 35 provenance、两个 VM 的导入准入与对象 reader | 29 场景在 Wasmtime/WAMR 的正常、挂起销毁、首次恢复后销毁模式运行；另测预取消、异常别名跨 GC、source 释放、错误快照和版本篡改；Task、waiter、frame、root 在 owner 退出后释放，静态槽在执行域卸载后释放 |
| 4. 生产入口 | CLI、缓存、generated facade、UHT 生成类型、Editor、包读取 | 同一源/绑定包通过正常入口构建并加载；真实 Actor/World、双实例、reload 成功/失败和 Blueprint 子类执行；专项开关不再是唯一可用路径 |

步骤 2 的原始 29 场景当前只使用 `AvidCancellationSource`，**并未**自然读标准 `CancellationToken` 属性。因此需要保留这 29 个场景做组合回归，另用同一业务源码追加观察适配层或独立场景证明 token 属性身份；不能把原始 29/29 直接写成 token 读取通过。若 .NET 参考侧的 Avid facade 无法给出同等 token 身份，先冻结比较规则与参考适配，不能降低 Guest 断言。

每步记录源/IR/WASM hash、完整通过数、负例、退出码和运行模式。编译器与原生 Automation 证据不能替代真实 Editor Play；Windows Shipping 和跨语言模块仍是后续独立 Gate。任何一个步骤暴露无法保持旧版兼容或所有权时，停止默认入口开放，修合同与迁移规则，不靠扩大白名单绕过。

## 对后续迭代的约束

P66.D 的玩法集成必须使用新组合产物，并测编译、加载、每帧 crossing、分配和取消/错误恢复成本。P67 的类型热更按字段/方法身份做迁移，不能将某语言的对象布局写入公共存档。P68 的调试帧绑定源码版本、模块代次和异步因果链。第二语言原型优先 Rust：由同一模块接口生成 C# 与 Rust SDK，完成双向调用、结构/数组/字符串、回调、挂起取消、UE handle、reload 与故障隔离；它不要求 Rust 实现 C# 专有异常能力。

领先声明另按[迭代路线图](AvidScript_Iteration_Roadmap.md)与[P65 性能协议](../Phase65/P65.D1_Performance_Leadership_Protocol.md)验收。能力组合首先解决**可表达与可维护**，不自动证明运行更快；性能优化必须保留等语义、安全检查和两种竞品的正式对照。
