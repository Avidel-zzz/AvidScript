# 开发体验与成熟度评估：AvidScript、Unreal AngelScript、Puerts

## 当前复核：2026-09-20

本节更新到产品提交 `f32b3fc6` 及本次只读检查到的工作树；后文 `f1b217e4`、29/29 等记录属于此前评估，不能当作本次执行结果。本次重新读取官方竞品资料、生产所有权、编译器拒绝分支和编辑器重载策略，未运行新编译、Automation、竞品计时或人工玩法验收。现存包级 prepared reload 改动尚未提交，本文不为它补充通过声明。Harness 显示 P66.A 完成、P66.B 进行中，protected dirty 7/7 保持。

**当前判定仍是开发者预览：有值得继续投入的基础，尚不满足成熟框架或开发效率领先的条件。** 最近实例上下文、绑定寿命和实例执行状态的改进，解决的是共享执行域的前置问题；不能据此推导普通跨对象 C#、持久事件、跨 await 闭包或完整调试已可用。P57–P65 的单项能力及打包证据，也不能替代这些组合能力。

| 决定开发体验的问题 | 本次源码证据 | 对产品的含义 |
| --- | --- | --- |
| 对象之间能否自然共享状态并相互调用 | `FGeneratedTypeRuntimeInstance` 持有独立 Session；Host 为实例创建 Session；Session 仍独占 `LiveRuntime` | 生产共享 VM/堆/静态状态、完整跨实例方法派发仍须完成；单个 Runtime 的共享探针不能代替生产路径 |
| 闭包是否可以同时参与事件、await 与调试 | `CSharpGuestLowerer` 明确拒绝尚未连接的 persistent async roots；启用 debug instrumentation 时拒绝 managed delegates | 语言组合和调试共享同一个保活缺口，必须一起设计根、帧、取消及版本寿命 |
| 增加反射字段是否无需重启 | `ApplyPublishedDescriptor` 的 `NativeRebuildRequired` 分支明确要求 no-clean 构建并重启 Editor | 现有方法体重载不能代表结构热重载；这是与成熟编辑循环的直接差距 |
| 任意玩法函数能否暂停检查 | `GetDebugResumableFunctionIds` 限定同步 void export，排除 EndPlay、async 和 Guest 调用目标 | source map、调试面板与顶层探针尚不能组成完整源码调试 |
| 重载失败能否保留原运行状态 | 工作树新增全包准备、复核、发布和逆序丢弃；原 VM 延后替换 | 方向正确，但待交付验证；原生观察者仍可能看见候选暂态，成功迁移也不包含任意堆/闭包 |

源码：[生成 Host](../../Source/AvidScriptRuntime/Private/ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.cpp)、[Session 所有权](../../Source/AvidScriptRuntime/Public/AvidScriptRuntimeSession.h)、[语言及调试入口](../../Tools/AvidScript.CSharpGuest/Lowering/CSharpGuestLowerer.cs)、[结构重载策略](../../Source/AvidScriptEditor/Private/GeneratedTypes/AvidScriptEditorGeneratedTypeReloadPolicy.cpp)。

竞品复核：Hazelight 的 Unreal AngelScript 有已发行游戏实践、编辑器脚本重载、Blueprint 子类和 VS Code 调试；官方区分“无需重启 Editor”与“PIE 中非结构变更”。它包含引擎修改，比较部署成本时必须计入。Puerts 的 UE 文档提供反射 API 接入、TS 类型继承、增量编译、热重载与 VS Code 调试；这不等于任意 npm 包、任意结构变更或全部平台无条件可用。来源：[AngelScript 概览](https://angelscript.hazelight.se/)、[Puerts UE 手册](https://puerts.github.io/en/docs/puerts/unreal/manual/)、[自动绑定](https://puerts.github.io/en/docs/puerts/unreal/uclass_extends/)、[调试](https://puerts.github.io/en/docs/puerts/unreal/vscode_debug/)。本次未实际操作两种竞品，不给出体验计时排名。

### 架构判断与下一步

保留 WASM、版本化 IR、ObjectHandle、生成绑定及候选验证。它们有实际工程价值。最大的投入风险是自建 C# 执行子集、对象系统、库和调试器，却让使用者不断迁就缺失语义。Roslyn 完成前端语义分析，不会自动提供这些执行能力；双 VM 后端也不会自动提高语言成熟度。

继续推进的首要单位应是一段自然玩法，而不是一个新增 opcode：A 调用 B、共享引用和虚派发，事件捕获状态，await 挂起，owner 销毁，重载失败，再定位故障。每个步骤必须同时验证值、对象身份、资源释放与源码诊断。先完成生产执行域和持久根；沿既定 P66 批次补语言组合。同步提前验证 P67 结构重载、P68 可暂停帧的关键假设，避免语言全部扩展后才发现执行模型无法支持编辑与调试。此建议不修改已冻结 Phase 状态。

建议将“跨时代”收敛为可检验的产品目标：**开发者能在运行中的游戏里安全修改玩法，看到对象和异步任务的因果关系，并在候选失败时继续使用原版本。** 超出普通热重载的价值来自这些能力的组合，而不是 C# 或 WASM 名称本身。受控重现和人/AI 修改预检可以后续共享同一套副作用边界，但目前仍是探索目标。

Windows 验收分三层：第一层，独立开发者自然完成技能、UI、网络、存档到 Shipping，无需特殊改写；第二层，同需求对测 AngelScript/Puerts 的开发、修改、排障耗时；第三层，再证明状态迁移、异步因果与安全修改带来的独有价值。沿用后文拟定的保存到生效 P95、迭代和定位耗时目标；另记录峰值内存、回收暂停及失败恢复，不能只比较标量 microbenchmark。当前不足以给出可信的“成熟完成百分比”或确定完成日期。

## 结论与证据边界

核对日期：2026-09-19。本次复核产品基线提交：`f1b217e4`；Harness 确认 P66 implementing、P66.A 完成、P66.B 进行中，7 项 protected dirty 基线保持一致。本报告是设计与实现差距评估，不修改 Phase 完成状态，不作为 Release Gate。

**AvidScript 是已有可运行链路的开发者预览，尚不是成熟、全面领先的脚本框架。** UE Binding、Session、WASM 执行与 Win64 打包已有较强基础；普通语言组合、结构修改的编辑循环、完整调试和长期项目使用仍有实质缺口。P66.A 已完成，P66.B 尚未完成，P66.C/D 与 P67–P69 仍待推进。

2026-09-20 实现增量：在 [共享与逃逸闭包](P66.B_CSharp_Closure_Execution.md)、[C# 共享引用](P66.B_CSharp_Borrowed_References.md)和[单目标委托身份](P66.B_Delegate_Identity.md)之上，已交付[组合/移除](P66.B_Delegate_Lists.md)与[结构体实例委托](P66.B_Bound_Value_Delegates.md)。[普通引用类合同](P66.B_Reference_Class_Contract.md)及[对象执行](P66.B_Reference_Object_Execution.md)进一步连接支持范围内的构造、共享字段和实例委托；完整继承、初始化、持久任务与调试仍未完成。以下竞品资料与旧探针保持原复核日期；新增能力以各组报告为依据，不把历史测试数当作最新完整 Gate。

本次阅读当前源码、阶段状态及既有报告，并执行一次固定 SDK 8.0.416 的定向探针：

```powershell
& "$env:USERPROFILE/.dotnet/dotnet.exe" run --project Tools/AvidScript.CSharpGuest.Tests --configuration Release -- --closures
```

本次结果：退出码 0，`AvidScript.CSharpGuest.Tests.Closures: 29/29 passed`。该聚焦 runner 包含 .NET 参考结果与 Guest 编译/产物断言；本次未重新运行 UE 中的 WASM。最新交付报告记录完整 Guest 232/232、WASM backend 112/112、原生堆 13/13、UE 双后端 Automation 5/5 及 no-clean UBT 通过，见[委托身份报告](P66.B_Delegate_Identity.md)。这些是已提交组的历史执行证据，本次没有重跑 UE、Shipping 或性能矩阵，也不将它们当作当前完整 Phase Gate。

当前状态必须分层阅读：同步闭包、内部引用 IR 6/1.5、委托比较/组合/移除与结构体绑定已交付；支持范围内的普通引用类已能执行、绑定实例委托并[捕获 this](P66.B_Receiver_Capture.md)；`cb5f95ab` 进一步交付[当前 Session owner 的 UE 实例委托与 this 捕获](P66.B_Ue_Receiver_Execution.md)。跨实例、虚派发、持久事件、跨 await 委托和完整调试仍未连接。底层 opcode、UE 事件桥接与普通 C# 语言表达分别验收。

## 竞品基线

这里的 AngelScript 指 Hazelight 的 **UnrealEngine-Angelscript 集成**，不是单独的 AngelScript VM。官方说明该方案包含引擎修改；其优势不能直接解释为任意纯插件都能以相同成本实现。

| 维度 | Unreal AngelScript 官方能力 | Puerts 官方能力 | AvidScript 当前差距 |
| --- | --- | --- | --- |
| 语言与日常表达 | 面向 UE 的脚本语言与项目实践 | UE 使用 JS/TS，类型声明生成，标准语言工具链 | Roslyn 接受源码不等于 Guest 能执行；闭包、实例委托、异常/集合/异步组合仍需统一 |
| 编辑循环 | 脚本变更无需重启 Editor；PIE 内非结构变更可重载 | 文档提供热重载、TS 类与代理 Blueprint、Mixin | 方法体重载已有；反射结构变更目前仍需 UBT 与 Editor 重启 |
| 调试 | VS Code 语言服务、断点、变量、步进 | VS Code 调试及 UE 对接说明 | 已有 source map 与受控暂停；内部函数、返回值函数、异步和闭包调试未闭环 |
| UE 使用 | Actor/Component、Blueprint 子类、网络、测试框架 | UE API 类型声明、反射与静态绑定、Blueprint 集成 | 已覆盖大量 UE 路径，但要验证自然写法的组合，而非只验证单项桥接 |
| 产品成熟度 | 官方有已发布大型游戏与长期团队使用证据 | 有现成运行时、工具、文档与集成方案；本报告不据此虚构规模数据 | Demo、自动化与 Win64 包可用，缺少独立开发者长期完整项目证据 |

来源：[AngelScript 概览](https://angelscript.hazelight.se/)、[安装与引擎修改](https://angelscript.hazelight.se/getting-started/installation/)、[预编译与转 C++](https://angelscript.hazelight.se/cpp-bindings/precompiled-data/)、[Puerts 仓库](https://github.com/Tencent/puerts)、[UE 使用手册](https://puerts.github.io/en/docs/puerts/unreal/manual/)、[调试](https://puerts.github.io/en/docs/puerts/unreal/vscode_debug/)、[自动绑定模式](https://puerts.github.io/en/docs/puerts/unreal/uclass_extends/)。本日重新读取上述官方页面；AngelScript development-status 页面超时，不用其未取得内容补充结论。这是官方能力说明，不是本机竞品体验实测；正式实验须另冻结 commit 与配置。

不能把 AngelScript 的热重载描述扩大为“PIE 内任意结构变更无条件成功”；其官方首页明确区分非结构变更。也不能把 Puerts 的 Unity 多语言特性、全部 npm 包或所有运行时环境直接算作 UE 能力。

## 当前设计值得保留的部分

1. **版本化编译与绑定合同。** Roslyn → Semantic → Guest IR → WASM、Binding descriptor 与 Runtime 独立演进，有利于诊断、兼容和工具复用。
2. **显式对象与任务所有权。** ObjectHandle、Session、代际校验、取消与失效隔离，使生命周期行为有统一检查入口。
3. **候选验证与可回滚状态。** 已有热重载事务基础，可发展为运行中的安全编辑；只对明确建模和可迁移的状态承诺回滚。
4. **预备调用与减少跨边界成本。** 部分 UE 路径已有领先冻结 Puerts 基线的历史结果，值得继续使用真实 workload 验证。

这些是可发展的基础，不自动证明开发效率领先。WASM 不自动提供确定性回放、任意副作用回滚、完整 .NET 兼容或原生进程沙箱。

## 最重要的架构风险

**一、语言承诺与实现负担。** 当前实际上同时维护了 C# 子集编译器、受追踪对象系统和 UE 框架。Roslyn 解决语义读取，未替代执行语义、库、异常和调试实现。应发布可机器校验的语言/库支持合同：支持的普通写法保持 C# 语义，不支持的组合在源码位置明确报错。不得用持续改写样例掩盖缺口，也不承诺任意 NuGet。

**二、同一对象跨越多个生命周期。** 闭包环境、UE 订阅、await continuation、调试暂停帧和热重载状态必须共享明确的身份与保活规则。共享引用、普通源代码引用类 receiver 与当前 Session owner 的 UE receiver 已连接同步调用；跨实例、持久 async roots 和闭包 debug instrumentation 仍未接通。进一步源码复核发现生成入口参数形状、跨 Session 引用与同步重入也需要统一合同，见[方法路由设计](P66.B_Ue_Method_Routing_Design.md)。只补同步 lambda 无法完成这一层。

**三、结构热重载决定编辑体验上限。** 当前生成反射类型的方式仍依赖原生构建。P67 应先做最小原型：新增属性/函数、修改默认值、Blueprint 子类实例、GC、复制和 Cook；比较动态类型重建与稳定原生外壳。原型失败时公开必须退出 PIE/重启的边界，不靠 UI 隐藏重建成本。

**四、完整调试需要执行模型支持。** 当前暂停范围集中在顶层同步 void export。仅增加面板或 IDE 启动配置，不能支持跨 Guest 调用帧、非 void 返回、异步因果链与暂停期间 GC。P66 的帧/根合同应为 P68 预留稳定入口，P68 再交付真实调试工作流。

**五、托管对象成本需单独预算。** 当前受追踪对象经 Host ABI 访问，不能把早期标量微基准成绩外推为闭包密集玩法的速度。需要分别记录分配量、保活、回收暂停、Host crossing 和尾延迟，再决定优化对象布局或批处理。

代码与本地证据：[语言入口及明确拒绝](../../Tools/AvidScript.CSharpGuest/Lowering/CSharpGuestLowerer.cs)、[委托组合实现](../../Tools/AvidScript.CSharpGuest/Lowering/CSharpDelegateComposition.cs)、[结构修改要求重启](../../Source/AvidScriptEditor/Private/GeneratedTypes/AvidScriptEditorGeneratedTypeReloadPolicy.cpp)、[堆操作 Host 调用](../../Tools/AvidScript.WasmBackend/Codegen/WasmManagedHeapEmitter.cs)、[调试 Gate 范围](../Phase61/P61.E_Integration_Gate.md)、[当前实现边界](../../README.md#当前边界)。历史文档中的已修复缺口不继续作为 blocker。

## 设计判定与投入边界

建议保留现有分层，集中补强跨功能合同。版本化 IR、受检 ObjectHandle、Session 与 prepared binding 已有实际用途；没有证据支持此时替换 VM 或推倒绑定系统。最大的产品风险是继续扩展语言表面时，各功能仍有独立的保活、暂停与迁移规则，最终让开发者承担组合成本。

需要区分两种目标：**成熟**要求普通玩法可自然表达、失败可定位、安装到 Shipping 可重复；**开发领先**还要求完成同一需求和定位同一故障时更省时间。WASM、C# 语法和单项速度优势都不能独自满足后者。

P66 后续以完整生命周期用例牵引：同一捕获值被多个回调读写，委托组合/退订，任务挂起后 owner 销毁，重载成功或失败，最终不重复执行、不悬空、不永久保活。先冻结身份、持久根及版本处理策略，再沿当前批次实现。语言兼容测试应继续采用同一 C# 源码的 .NET 参考语义，明确不支持的标准库与组合，避免用专用样例绕开限制。

P67 的动态反射原型是关键决策点。若新增反射字段仍必须重启，应该先验证是否能改变这个约束，再扩大编辑器功能投入；不能把生成 C++ 外壳与增量编译包装成结构热重载。稳定外壳或动态类型都须保留 Blueprint 子类、GC、复制与 Cook 行为，其成本目前未知。

P68 应共享语言帧与根合同，提供内部函数、闭包及 await 的可检查状态。P69 再将它们交付为连续的玩法修改体验。受控重现、AI 修改预检与状态撤回可以成为后续差异化方向，但在副作用和迁移边界被证明之前，均属于待验证设想。

## “跨时代”应落实为怎样的体验

建议产品目标是：**开发者在运行中的游戏里修改玩法、理解异步与状态变化、验证结果，并安全地保留或撤回变更。** 第一阶段不另造语法，先让熟悉的 C# 表达可靠可用。

| 能力目标 | 用户可感知的结果 | 必须证明的边界 |
| --- | --- | --- |
| 运行中编辑 | 保存普通玩法修改后快速生效，尽量保留实例状态 | 哪类修改保持 PIE；迁移失败如何保留旧版本；网络两端如何兼容 |
| 有归属的异步 | 技能、UI 或 Actor 销毁后，关联任务与订阅自动结束 | 取消、异常、重入、await、reload 的组合测试 |
| 可解释执行 | 可从故障跳到源码、对象、await 来源与相关事件 | 内部函数、返回值、跨层调用栈和重载后映射真实可用 |
| 可复现验证 | 记录受控输入与事件，在测试世界重现问题 | 随机、时间、网络和外部副作用显式建模；不宣称整个 UE World 天然确定 |

最后一项属于后续探索，不加入当前 P66 完成声明。应先在小型受控 gameplay slice 验证成本，再决定是否扩展为产品方向。

## 从功能集合走向统一执行模型

当前最值得投入的方向，是把对象、任务、事件、调试与重载连接到同一套执行身份和生命周期合同。它们现在各有基础，但缺少组合闭环。以下是建议的设计审查要求，不是已经实现的新能力，也不改变已冻结批次：

| 共同合同 | 需要统一的行为 | 首个可证伪的用例 |
| --- | --- | --- |
| 对象与根 | 闭包环境、订阅、挂起任务、暂停帧都声明 owner、保活与释放责任；语言别名保持同一位置 | 事件捕获局部状态，await 中暂停后销毁 Actor；不能悬空、重复恢复或永久保活 |
| 版本与迁移 | 稳定字段身份与 schema 区分于具体编译产物；活动帧可以继续旧版本、取消或拒绝迁移，但必须明示策略 | 在技能等待期间改函数和状态字段；旧任务与新实例的行为有确定解释，迁移失败保留旧版本 |
| 外部作用 | 区分读取、可暂存/回滚写入和不可撤销外部作用；候选验证、回放和自动修改使用同一边界 | 重载准备期间禁止实际发送 RPC 或写存档；获准提交后才执行明确支持的作用 |
| 因果与诊断 | 将源码位置、调用帧、任务父子、事件来源、owner 和版本关联；明确记录成本及开关 | 从失败的 continuation 找到发起它的事件与已销毁对象；重载后仍映射到正确代码版本 |

不要试图序列化任意运行中栈或让整个 UE World 自动回滚。先证明同步/挂起边界上的受控迁移和诊断，再扩大保证范围。统一合同也不意味着把所有代码合并进 Runtime；编译器描述语义，IR 验证表示，Runtime 管理 Session，Editor 消费诊断与修改计划。

“AI 能改脚本”本身不足以形成领先。可以探索的后续产品体验是：人或 AI 提交修改后，系统展示受影响的实例/任务，执行受控验证，提交可迁移状态并保留失败原因。它依赖上述合同，不应先做一个聊天面板代替底层能力。

## 用一个真实修改循环检验方向

建议 P69 的首个对照任务采用同一 Windows 技能/UI 切片：按键启动技能，异步加载资源，Timer 等待，事件闭包累计命中，更新 UI，服务器确认结果并保存进度。随后依次修改伤害规则、增加状态字段、注入异步失败、在挂起期间销毁 Actor、定位并修复问题。

三套框架使用相同需求与正确性断言，允许各自惯用写法。计时包含编译、重启、手写桥接和排障，不只记录脚本执行耗时；先进行等量熟悉，再交叉执行任务，保存失败尝试。先记录 AvidScript 当前无法自然表达的步骤，不能等样例被改写成特殊形式后才开始比较。

这一切片同时回答三个不同问题：语义是否可靠、开发是否更快、结果是否能发布。只有第三方开发者可以独立完成，并在重复修改和长时间运行中保有优势，才具备宣称成熟和开发效率领先的依据。当前不提供“已完成百分比”，也不把 P65 的阶段编号当作产品成熟度。

## 推进优先级与验收

保留已经冻结的 P66–P69 顺序，不重新解释已完成批次：

1. **P66：完成日常语言语义。** 同步共享/逃逸闭包、内部 C# 引用、委托比较/组合/移除、结构体与受支持引用类实例绑定及 this 捕获已交付；当前 owner 的 UE 实例委托已有执行证据，接下来打通跨实例/虚派发、持久事件、跨 await 生命周期、集合和错误清理。执行能力测试同时包含真实 WASM 和与 .NET 参考结果的语义对照。
2. **P67：证明结构修改的编辑循环。** 先做 UE 原型，再决定实现；同时覆盖 Blueprint、GC、复制及 Cook，不能只测试裸脚本类。
3. **P68：补齐真实调试。** 内部函数、返回值、异步链、闭包、reload 后断点与变量，使用真实 IDE 工作流验收。
4. **P69：用真实 Windows 玩法决定成熟度。** 同一套技能、UI、存档和网络任务，比较首次实现、需求修改和故障定位；找未参与框架实现的开发者试用。

建议在实施前冻结以下对测指标。这些是拟定目标，不是已经达到的成绩：方法体小改动保存到生效 P95 ≤ 1 秒；同一完整任务迭代时间相对两种对照分别降低至少 30%；故障定位中位时间降低至少 50%；记录重启次数、手写桥接量与无法自然表达的用例。若固定机器和工程规模下目标不合理，应保留原结果并调整目标依据，不能删掉慢样本。

性能另设矩阵：统一语义、数据表示、优化和边界调用次数；分别比较 Editor 与 Shipping；AngelScript Shipping 包含其可用的转 C++ 路径，Puerts 包含 reflection/static 路径；报告 P50/P95、分配与回收。现有 README 记录的完整性能门槛为历史 16/18，通过部分不能改称全面领先；AngelScript 当前也没有已完成的同口径全矩阵。

成熟的最低判据是：普通开发任务无需为适配框架重写设计；诊断能解释失败；承诺的热重载和调试行为稳定；独立开发者能从安装走到 Windows Shipping。具备这些证据后，才评估是否在开发效率上领先。Android/Mac 继续暂缓，P65 剩余债务保持原状态。
