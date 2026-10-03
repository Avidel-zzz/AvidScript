# 可组合语言能力合同：P66.C 到跨语言模块

状态：P66.C 实现中，2026-10-04。Semantic 54 / IR 35 的静态 + token 组合、Semantic 55 / IR 36 的 `async void` 错误 owner、Semantic 56 / IR 37 的 owner 与静态/token 组合已有专项验证。完整版本与证据见 [P66.C11](../Phase66/P66.C11_Async_Void_Error_Owner_Contract.md)、[P66.C12](../Phase66/P66.C12_Async_Void_Composition_Contract.md)。默认入口和共享类型域尚未完成；既有 IR 字节不重新解释。

首个实现切片已在 Guest IR 中加入可选清单字段、精确执行基线/能力 ID 校验及 IR 35 严格 JSON 读取。旧版本不能携带新清单。Guest 验证器在未改写版本的手写 IR 35 夹具上核对同步 `managed.static_storage` + `error.cancellation_token_value`、14/1.13 基线、两种指令和 token 类型布局；缺计划、错基线、伪造静态槽、额外异步元数据及异常 token reader 均拒绝。现另准入精确的 29/1.28 五能力异步组合；其余组合仍拒绝。

C# 静态初始化不能简单映射到上述 14/1.13 夹具：现有编译器为“一次初始化、失败保留和边界报错”生成 IR 27、以 17/1.16 为基线，即使静态字段是 `int` 也带类型状态槽。IR 35 现在还可直接验证这个 17/1.16 基线的静态初始化模块与独立加入的 token 值操作，检查 outcome、错误目录和报告 import 的精确签名；专项 **13/13** 通过。该夹具保留原 IR 27 的来源身份并标为 `guest-ir`，不是从同一份 Semantic 54 C# 源码 lowering 的产物。

第二切片已让 C# 前端在显式启用静态初始化、async 异常和标准 token 分析时，从同一份源码投射 Semantic 54/1.63。能力清单按实际投射的字段和操作生成，不因 generated facade 的未调用签名登记了 token 类型就声明 token 能力。同步静态字段 + token 源码使用两项能力及 Semantic 31/1.40 基线；含 await、取消身份与异常计划的源码使用五项能力及 Semantic 50/1.59 基线。清单和静态计划的基线必须一致；其他尚未验证的组合显式拒绝。旧 Semantic 53 不写清单；Semantic 54 的未知/重复 JSON 字段、旧版夹带清单、缺失能力和冲突基线均拒绝。该切片固定 SDK 的 Semantic 全量 1516/1516、Guest IR 409/409、WASM backend 417/417、C# Guest 回归 4506/4506 通过。

第三切片让同一份**同步** C# 源码中的静态字段初始化和 `CancellationToken.None` 值操作降到 Guest IR 35/1.34、17/1.16 执行基线。编译器先验证原始 Semantic 54，再在私有执行副本中补齐合成初始化方法的 `void` 类型、静态 guard 和 token 值布局；发布的 IR 保留原始 Semantic 54 的来源 hash 和两项能力清单。规范序列化、反序列化、重复编译与缺能力负例均通过；原始源码不含 `void` 方法的静态字段回归还检查了原有 IR 27 的 WASM 编译。该切片完成时固定 SDK 的同源专项 **20/20**、静态源码专项 **223/223**、C# Guest 全量 **4533/4533** 通过。

第四切片接通 IR 35 的 WASM emitter 和原生 provenance reader。新产物写入 `guest_ir=35/1.34`、`execution_base`、规范能力清单、`source_language` 和源语义版本；原生读取只准入 14/1.13 或 17/1.16 上的两项同步能力，检查模块身份、来源 hash、精确字段集和错误目录。固定 SDK 的 Guest IR **410/410**、C# Guest **4537/4537**、WASM backend **419/419**、Win64 no-clean Editor 构建及聚焦 Automation **2/2** 通过。同源 17/1.16 WASM SHA-256 为 `f98ca31778c5c6691e1caade3dc52e33fb8f28f7665564576b199176abf59516`；Wasmtime JIT 与 WAMR Interpreter 各在两个新执行域中连续返回 `2`、`3`，共 **4/4** 次观察，卸载后堆释放。可用 `Build/TestAvidScriptComposableIr35.ps1` 重现，原始日志位于工程 `Saved/AvidScriptComposableIr35/20260928T022220003Z/`。这仍未覆盖异步五能力、正常编辑器构建入口或真实 Play。

第五切片让原生 provenance reader 准入**精确**的 29/1.28 五能力、C# Semantic 54/1.63 和取消生命周期字段；缺能力、乱序、错基线、错来源、错模块身份、缺错误目录及未知字段均拒绝。`Build/TestAvidScriptComposableIr35.ps1` 重新生成的异步 WASM（SHA-256 `cf49263136f63d5e5d2322ab6f67773d7415c781f32700287808b700c66732e8`）已通过原生错误目录读取和授权测试；固定 SDK 的组合专项 **33/33**、Win64 no-clean Editor 构建、聚焦 Automation **2/2** 通过，同一轮的同步双 VM 观察仍为 **4/4**。原始证据在工程 `Saved/AvidScriptComposableIr35/20260928T083904291Z/`。

同一产物随后在 Wasmtime JIT 和 WAMR Interpreter 均通过 Host import 准入、实例化和错误/取消能力授权；Win64 no-clean 构建与聚焦 Automation **3/3** 通过，证据在工程 `Saved/AvidScriptComposableIr35/20260928T085305465Z/`。此处尚未调用异步 `BeginPlay`、恢复 continuation 或验证玩法结果。

第六切片在同一业务源码中追加只读结果导出，异步 WASM SHA-256 变为 `46262e2708beff31d26e95ac642549f62d20eb38644b8f7e6d46a616a6efda06`。Win64 Wasmtime JIT 与 WAMR Interpreter 各自调用 `BeginPlay`，观察挂起时结果 `0`，通过真实 World/Session 下一帧恢复后读到初始化静态状态 `1`，continuation 和调用帧归零；异步观察 **2/2**，原同步观察 **4/4**。固定 SDK 组合专项 **34/34**、no-clean Editor 构建、聚焦 Automation **3/3** 通过，结果与日志位于工程 `Saved/AvidScriptComposableIr35/20260928T090545205Z/`。本轮只覆盖正常完成路径，取消、异常、reload、原始 29 场景和真实玩法仍待验收。

第七切片在同一源码的 `Task<int> Run` 中创建取消源、读取标准 `CancellationToken` 并等待下一帧。正常完成返回静态字段 `1`；在恢复前取消时，`catch (OperationCanceledException)` 比对原 token，再首次读取静态字段并返回 `7`，身份不符会返回 `9`。Win64 Wasmtime JIT/WAMR Interpreter 各执行两条路径，**4/4** 观察得到 `1/7`，EndPlay 后 continuation、Task 和取消源均为零。固定 SDK 组合专项 **34/34**、no-clean Editor 构建、聚焦 Automation **3/3** 通过；最终 WASM SHA-256 为 `510cbba5323649c4996dc4e22d7da663d009c5e484327dceaae77fa5d4e01498`，证据在工程 `Saved/AvidScriptComposableIr35/20260928T093521654Z/`。这验证了同一条取消执行路径上的 token 身份、异常捕获和静态初始化，不代表原始 29 场景、挂起态 teardown、reload 或默认构建入口已通过。

第八切片在同一 WASM 中增加第三条执行路径：`BeginPlay` 挂起后立即调用 `EndPlay` 并 teardown Session，不派发取消完成。两个 VM 均在挂起时观察结果 `0`，teardown 后 ready callback、continuation、Task、取消源和调用帧为零；加上正常与显式取消，共 **6/6** 次异步观察。固定 SDK 组合专项 **34/34**、no-clean Editor 构建和聚焦 Automation **3/3** 通过，证据在工程 `Saved/AvidScriptComposableIr35/20260928T094600201Z/`。这是独立 Session 的 EndPlay 路径；真实 Actor 销毁、World teardown、生成类型共享域和 reload 仍需分别验证。

第九切片保留 `AwaitMemberAssignment.cs` 原始业务字节，先将 `field-mode-0` 接入 Semantic 54 → IR 35。测试适配层单独加入异常对象及标准 token 读取，使五项能力都由源码产生。修正了同时存在源声明 `object` 与托管引用时的重复类型登记，并让 IR 35 直接核查原源码中的显式 throw owner/transfer；删除这些 transfer 的负例被拒绝。该场景的 9 个成员 await 经编译，.NET 参考值 `Result=7`、`Trace=1238` 与 18 个字段在 Win64 Wasmtime JIT/WAMR Interpreter 的正常执行一致；两个 VM 还实际读取到非 `None` token，挂起及首次恢复后 teardown 也通过，共 **6/6** 次观察。固定 SDK 组合专项 **41/41**、no-clean Editor 构建、聚焦 Automation **4/4** 通过，证据在工程 `Saved/AvidScriptComposableIr35/20260928T101002432Z/`。这是原始 29 场景中的 **1 个**；其他 28 个、Task 终态逐项对照和真实 Actor/World 生命周期尚未完成。

第十切片将同一份未改动的业务源码与 29 个原始执行适配场景全部投射为 Semantic 54 五能力、直接验证 IR 35 并编译 WASM；适配层新增的命名 `catch` 保持原返回值与 `Status`，标准 token 探针独立于业务路径。.NET oracle 为每例提供 18 个字段、Task 初始/终态与异常类型、循环挂起快照。Win64 Wasmtime JIT/WAMR Interpreter 在正常执行、初始挂起 teardown、首次恢复后 teardown 上完成 **174/174** 条路径，聚焦 Automation **4/4**、固定 SDK 组合专项 **43/43**、旧 IR 31 原始矩阵 **391/391** 通过；证据在工程 `Saved/AvidScriptComposableIr35/20260928T102443457Z/`。这些模式使用独立 Session 与测试 World，尚不证明真实 Actor/World 销毁、reload、`async void` 未处理错误或默认 Build And Bind。

第十一切片为 29 个原始场景各生成与实际 WASM 的 import/export 和 SHA-256 匹配的 manifest，并让全部场景走正式 `UAvidScriptComponent`。Win64 Automation 为每个场景创建真实 Actor 和测试 World，分别销毁 Actor、结束 World；WAMR 字节码与 Wasmtime serialized AOT 各覆盖两种销毁，共 **116/116** 条 Component 路径。按同源 Task oracle 区分初始挂起与已终态，分别为 **76** 和 **40** 条；挂起路径确认 Task、等待者、continuation 和状态帧非零。所有路径在销毁后确认 Component 收到 EndPlay、Session 释放、旧 VM 租约失效；Actor 销毁后额外推进 4 帧，没有新的脚本 Tick。同轮原始 29 场景独立 Session 对照 **174/174**、固定 SDK 组合专项 **72/72**、聚焦 Automation **5/5**，Win64 no-clean Editor 构建通过；证据在工程 `Saved/AvidScriptComposableIr35/20260928T112919316Z/`。Component 矩阵验证的是加载与销毁，不替代独立 Session 的字段/Task 终态对照；双实例、reload、`async void` 未处理错误、默认 Build And Bind 和真实 Editor Play 仍未完成。

第十二切片在正式 `UAvidScriptComponent` 上使用原始业务源码的 `field-mode-0/1` 两份测试适配产物。双 Actor 起始各有独立、挂起的 Task/VM；销毁其中一个后，另一个继续运行并得到 .NET 预期 `Result=7, Trace=1238`。挂起态重载另验证两种失败位置：候选 WASM 哈希错误在加载时拒绝，哈希正确但缺少必需导出在 VM 校验时拒绝。两次拒绝都保留原 Session、VM、Task、等待者和状态帧；只经历校验失败的 Actor 随后完成旧版本，仍得到 `7/1238`。另一 Actor 成功切换到 `field-mode-1`，旧 VM 租约失效，新版本得到 `7/128`；结束 World 后两边租约均失效。WAMR bytecode 和 Wasmtime serialized AOT 分别通过双实例 **2/2**、挂起重载 **2/2**，完整原始矩阵 **174/174**、Component 销毁 **116/116**、固定 SDK 专项 **72/72**、Automation **7/7**；同一候选源码的 Win64 no-clean Editor 构建通过。证据在工程 `Saved/AvidScriptComposableIr35/20260928T121505140Z/`，入口为 `Build/TestAvidScriptComposableIr35.ps1`。这里的失败发生在加载/VM 校验阶段；没有证明候选 `BeginPlay` 已执行后失败的回滚、辅助类静态状态迁移、生成类型共享执行域或真实 Editor Play。

第十三切片在同一个 Component 挂起重载用例中，让原始 C# `field-mode-1` 候选执行 `BeginPlay` 后、发布前失效。测试钩子在执行前经 Host effect journal 保存 Actor Transform 并改变位置，执行后卸载候选 VM 以拒绝提交；两种后端均确认事务恢复位置、候选 VM 租约失效、旧 Session/VM/Task/等待者/状态帧保留，随后旧任务与新版本任务仍分别得到 .NET 预期结果。固定 SDK 专项 **72/72**、原始矩阵 **174/174**、Component 销毁 **116/116**、双实例 **2/2**、挂起重载 **2/2**、Automation **7/7** 与 Win64 no-clean Editor 构建通过；证据在工程 `Saved/AvidScriptComposableIr35/20260928T134806832Z/`。这里的 Host 变更和候选失效均由测试钩子注入，尚未证明 C# 语言错误在 `async void BeginPlay` 中自然触发同一回滚，也未覆盖 Guest 发起的 UE 变更、静态状态迁移或真实 Editor Play。

## 现有阻塞

[SemanticAnalyzer](../../Tools/AvidScript.CSharpSemantic/Analysis/SemanticAnalyzer.cs)可投射版本化的静态、async、token 和 `async void` 组合。原始 IR 35 的 [29 个成员 await 场景](../Phase66/P66.C10_Await_Member_Assignment_Contract.md#同源基准矩阵)已通过同源对照、挂起态 teardown 和正式 Component 双 VM Actor/World 销毁矩阵；IR 36 已验证语言错误自然触发的独立 Session 候选回滚。IR 37 又验证了 24 份静态/token/owner 组合源码及 192 条双 VM 生命周期路径。共享生成类型域的组合错误隔离/回滚、默认入口、真实 Editor Play 和玩法仍待验证。

早期 `async void` 缺少同步错误 owner 的问题已由 IR 36/37 合同处理。IR 37 同源场景在 `async void Run` 内直接创建取消源，执行标准 token await 和本地 catch；未处理错误经受检报告交给 Session，finally 和错误根释放先完成。该源码/Runtime 专项需要显式选项，默认 Build And Bind 仍未启用。

token reader 验证器现可在真实 IR 35 输入上识别 29/1.28、Semantic 54、异常值与 token 值两项声明的组合，并拒绝缺声明、旧来源或同步 token 计划。同源候选先核对旧包装生成的静态、readiness、取消身份、token 与异常计划链，再把 readiness 和取消身份计划统一标记为 29/1.28；各独立验证器随后按原始 IR 35 模块检查执行路径。计划字段一致本身不能替代这一步。

静态槽验证器也已在 IR 35 上检查 29/1.28 的非空有界槽表与引用类型，并拒绝错配基线。该局部通过不覆盖异步初始化 guard 的错误路由和 owner 验证。

托管堆验证器已按五能力计划和 Semantic 54 来源识别 IR 35 的规范错误报告、Task fault、根读取与取消错误 import；五种 import 的名称别名均拒绝。这个准入只处理模块内受追踪引用的导入签名，Task/取消的调用点和生命周期仍交由独立验证器检查。

相同来源与计划判断现供 outcome 类型、调用流和错误目录验证器使用。同源候选的 outcome 形状与状态分支检查通过；缺目录或 outcome 类型的 IR 35 负例被拒绝。IR 35 现在还用真实模块核查同步异常转入 Task 的计划站点，错误目录据此追溯 async token；缺计划和伪造站点仍拒绝。这不是旧 IR 28 的 `TaskErrorTransfers` 计划，后者不由当前 C# lowering 生成。

Task<int> 与 Task 语言错误验证器现按同一 IR 35 声明检查 Host 导入和错误根签名。同源候选通过这两项；缺失或错误签名的 Task/fault 导入仍拒绝。取消错误使用 terminal read 导入对，不强制附加旧式 Task read 导入对；terminal read 的完整性仍须由取消验证器单独核对。

异步异常路由、直接 `await` 路由和 readiness guard 现可在真实 IR 35 候选上校验恢复函数、状态分支、类型化取消 owner 与取消写入。readiness guard 验证通过后才把其取消写入登记为已检查的 producer；直接路由验证器继续拒绝未登记写入。同源候选不再报 `ASIR1030/1031/1038`。

取消错误验证器现直接核查 IR 35 的 v2 取消写入、terminal metadata/root 导入、目录 token 和新根；同源候选不再报 `ASIR1032`。缺少 terminal metadata 或写入签名错误的 IR 35 负例仍被拒绝。

Task local 生命周期验证器现以 Semantic 54/1.63 为来源直接检查 IR 35 的 owner 槽、retain/release/transfer helper、释放位置及 scope-exit 边；异常转移验证器使用已核对的同一执行模块。同源候选不再报 `ASIR1033`，缺 Task lifetime 计划的 IR 35 仍被拒绝。

取消身份验证器现按 IR 35 的 29/1.28 计划核对 v2 writer、v1 reader、已验证取消 producer 和 readiness token；同源候选不再报 `ASIR1039`。缺 reader 或参数类型不符的 IR 35 负例仍被拒绝。

异常值验证器现允许有准确五能力声明的 IR 35 同时携带静态存储，仍逐项校验捕获块、Task 错误根、类型匹配和引用别名。同源候选不再报 `ASIR1040`；空绑定或伪造捕获点被拒绝。精确五能力清单、完整 async 元数据和 29/1.28 基线通过 `ASIR1042`，缺能力或缺异常转移计划被拒绝。同源模块可规范往返，重复编译的 Guest IR / WASM 字节一致；原生 provenance/错误目录及取消分支的异常 token 身份已在双 VM 验证。

旧版组合依靠外层版本和 `BaseProfile` 回退到旧执行版本；IR 35 同步及精确异步五能力模块已由 [GuestValidationContext](../../Tools/AvidScript.GuestIr/Validation/GuestValidationContext.cs)直接验证真实输入，并由 [WASM provenance](../../Tools/AvidScript.WasmBackend/Codegen/WasmModuleCompiler.cs)写入执行基线与能力清单。[原生读取者](../../Source/AvidScriptRuntime/Private/Diagnostics/AvidScriptLanguageErrorCatalog.cpp)已接受同步两能力及精确异步五能力的来源/错误目录，其他组合仍拒绝。继续增加外层版本会使前端、缓存和 Host 的组合数量随能力增长。

异步组合按以下依赖推进；每一行在真实 IR 35 上验证，不通过改写版本字段取得旧验证器的通过结果：

| 顺序 | 必须完成的合同 | 验收 |
| --- | --- | --- |
| 1. 来源与恢复边 | 同源 Semantic 54 形成私有执行副本；静态 guard 在 exported async 入口和恢复段都有异常目标与 owner；无目标的调用继续拒绝 | 静态初始化成功/失败、await 前后访问及导出入口均能保留正确错误路由 |
| 2. 统一执行基线 | 静态槽、readiness、取消身份与 token reader 全部标记 29/1.28；原始 29 场景已用真实 IR 35 直接验证 | 计划的 base 一致，旧 IR 31/34 序列化与执行不变，错 base 负例拒绝 |
| 3. IR 35 直接验证 | outcome/catalog、Task 结果与错误、async route/transfer、静态堆、托管根 import、reader 和五项增量在同一个 artifact 上逐项校验 | 原始 29 场景已通过；仍需扩展移除计划/import、错签名、伪造来源及旧版夹带能力的组合负例 |
| 4. WASM 与原生准入 | emitter 和 Host reader 接受准确的 29/1.28 五能力 provenance；两 VM 执行相同字节 | 原始 29 场景、独立 token 身份、Component 销毁、双实例及加载/VM 校验阶段重载已通过；继续验证候选执行失败、生成类型共享域、错误快照和默认入口 |

## 合同边界

| 层 | 负责什么 | 不负责什么 |
| --- | --- | --- |
| 语言前端 | C# 用 Roslyn 证明源码语义、生成确定性的功能计划；后续语言有自己的前端版本 | 不把 C# 的异常对象或堆布局当公共 ABI |
| Guest IR | 规定执行指令集、能力集合、各能力的计划与依赖；一个模块只验证一次规范化结果 | 不信任前端声明就放行缺失的计划或 import |
| WASM/Host | 将已验证能力写入产物与 provenance；Runtime 按产物版本和授权导入执行 | 不从 WASM 名称猜能力，也不允许未知导入试探 |
| 跨语言模块接口 | 另行定义数据、资源、调用、异步和错误的稳定交换合同 | 不直接共享各语言的 Task、异常对象、GC 根或闭包内存 |

IR 35 将 `execution_base` 与**排序、去重、版本化**的 `capabilities` 列表分开。清单结构识别 14/1.13、17/1.16 与 29/1.28 基线；同步静态状态 + token 的 14/1.13 槽夹具、17/1.16 静态初始化夹具及 29/1.28 的原始 29 场景已通过真实模块验证和双 VM 执行。纯计算或同步模块不被迫携带 async 基线。基础指令集与能力版本是两条轴：后续增加一个可独立验证的能力，不再为所有已有能力的排列新增外层 IR。现有计划结构可以在首轮复用，但其 `BaseSchemaVersion/BaseIrVersion` 必须全部等于所选执行基线；新合同不允许多个互相矛盾的 base。规范化后只用真实输入模块做授权与所有权验证，不通过改写版本字段的临时视图绕开新组合。

`execution_base` 自身已有任务、错误、所有权和指令合同；能力列表只列可独立组合的增量，不把 IR 29 固有计划重新声明一遍。各验证器仍检查基线与增量的全部计划、指令和 import，不能因为一项未列在增量清单中就跳过基线校验。

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

WASM 的新 provenance 同时记录 `guest_ir=35/1.34`、`execution_base`、规范能力列表、`source_language` 和 `semantic`。原生错误目录读取者准入 14/1.13 与 17/1.16 的同步两能力组合，以及 29/1.28 上的精确异步五能力组合；这份异步产物的双 VM 正常挂起恢复及取消 token 身份已通过，其他异常形态仍须验证。旧 IR 1–34 保持原有读取格式，不能把新字段塞进旧 IR。Host 验证实际加载的 WASM 与包中已审核的 hash、provenance 和 import 表一致，再按 Session、World、线程、根与代际检查执行。未知必需能力、未来版本、错误签名、别名 import 和能力/计划/导入不一致均 fail closed。

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
