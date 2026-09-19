# 开发体验与成熟度评估：AvidScript、Unreal AngelScript、Puerts

## 结论与证据边界

核对日期：2026-09-19。本地基线提交：`933ca0a41ae942bc68e20135d5babfc947bebd2b`；工作树另含尚未验证的 P66.B 闭包执行改动。本报告是设计与实现差距评估，不修改 Phase 完成状态，不作为 Release Gate。

**AvidScript 是已有可运行链路的开发者预览，尚不是成熟、全面领先的脚本框架。** UE Binding、Session、WASM 执行与 Win64 打包已有较强基础；普通语言组合、结构修改的编辑循环、完整调试和长期项目使用仍有实质缺口。P66.A 已完成，P66.B 尚未完成，P66.C/D 与 P67–P69 仍待推进。

本次阅读当前源码、阶段状态及既有报告，并执行一次固定 SDK 8.0.416 的定向探针：

```powershell
& "$env:USERPROFILE/.dotnet/dotnet.exe" run --project Tools/AvidScript.CSharpGuest.Tests --configuration Release -- --closures
```

结果：退出码 1，`CSharpManagedDelegateLowerer.TryLowerInvocation` 第 86 行发生 `NullReferenceException`。这是当前未提交闭包实现的编译期失败，没有得到 WASM 执行成功证据；不据此否定之前已提交能力，也不把旧测试数量算作当前 tree 的通过结果。本次未重跑全量 UE、Shipping 或性能矩阵。

## 竞品基线

这里的 AngelScript 指 Hazelight 的 **UnrealEngine-Angelscript 集成**，不是单独的 AngelScript VM。官方说明该方案包含引擎修改；其优势不能直接解释为任意纯插件都能以相同成本实现。

| 维度 | Unreal AngelScript 官方能力 | Puerts 官方能力 | AvidScript 当前差距 |
| --- | --- | --- | --- |
| 语言与日常表达 | 面向 UE 的脚本语言与项目实践 | UE 使用 JS/TS，类型声明生成，标准语言工具链 | Roslyn 接受源码不等于 Guest 能执行；闭包、实例委托、异常/集合/异步组合仍需统一 |
| 编辑循环 | 脚本变更无需重启 Editor；PIE 内非结构变更可重载 | 文档提供热重载、TS 类与代理 Blueprint、Mixin | 方法体重载已有；反射结构变更目前仍需 UBT 与 Editor 重启 |
| 调试 | VS Code 语言服务、断点、变量、步进 | VS Code 调试及 UE 对接说明 | 已有 source map 与受控暂停；内部函数、返回值函数、异步和闭包调试未闭环 |
| UE 使用 | Actor/Component、Blueprint 子类、网络、测试框架 | UE API 类型声明、反射与静态绑定、Blueprint 集成 | 已覆盖大量 UE 路径，但要验证自然写法的组合，而非只验证单项桥接 |
| 产品成熟度 | 官方有已发布大型游戏与长期团队使用证据 | 有现成运行时、工具、文档与集成方案；本报告不据此虚构规模数据 | Demo、自动化与 Win64 包可用，缺少独立开发者长期完整项目证据 |

来源：[AngelScript 概览](https://angelscript.hazelight.se/)、[开发状态及限制](https://angelscript.hazelight.se/project/development-status/)、[安装与引擎修改](https://angelscript.hazelight.se/getting-started/installation/)、[Puerts 仓库](https://github.com/Tencent/puerts)、[UE 使用手册](https://puerts.github.io/en/docs/puerts/unreal/manual/)、[调试](https://puerts.github.io/en/docs/puerts/unreal/vscode_debug/)、[Mixin](https://puerts.github.io/en/docs/puerts/unreal/mixin/)。网页为本日读取的可变资料，正式实验须另冻结 commit 与配置。

不能把 AngelScript 的热重载描述扩大为“PIE 内任意结构变更无条件成功”；其官方首页明确区分非结构变更。也不能把 Puerts 的 Unity 多语言特性、全部 npm 包或所有运行时环境直接算作 UE 能力。

## 当前设计值得保留的部分

1. **版本化编译与绑定合同。** Roslyn → Semantic → Guest IR → WASM、Binding descriptor 与 Runtime 独立演进，有利于诊断、兼容和工具复用。
2. **显式对象与任务所有权。** ObjectHandle、Session、代际校验、取消与失效隔离，使生命周期行为有统一检查入口。
3. **候选验证与可回滚状态。** 已有热重载事务基础，可发展为运行中的安全编辑；只对明确建模和可迁移的状态承诺回滚。
4. **预备调用与减少跨边界成本。** 部分 UE 路径已有领先冻结 Puerts 基线的历史结果，值得继续使用真实 workload 验证。

这些是可发展的基础，不自动证明开发效率领先。WASM 不自动提供确定性回放、任意副作用回滚、完整 .NET 兼容或原生进程沙箱。

## 最重要的架构风险

**一、语言承诺与实现负担。** 当前实际上同时维护了 C# 子集编译器、受追踪对象系统和 UE 框架。Roslyn 解决语义读取，未替代执行语义、库、异常和调试实现。应发布可机器校验的语言/库支持合同：支持的普通写法保持 C# 语义，不支持的组合在源码位置明确报错。不得用持续改写样例掩盖缺口，也不承诺任意 NuGet。

**二、同一对象跨越多个生命周期。** 闭包环境、UE 订阅、await continuation、调试暂停帧和热重载状态必须共享明确的身份与保活规则。当前源码仍拒绝捕获 receiver、未接通的 async roots、捕获 cell 的普通 ref/out 适配，以及闭包 debug instrumentation。只补同步 lambda 无法完成这一层。

**三、结构热重载决定编辑体验上限。** 当前生成反射类型的方式仍依赖原生构建。P67 应先做最小原型：新增属性/函数、修改默认值、Blueprint 子类实例、GC、复制和 Cook；比较动态类型重建与稳定原生外壳。原型失败时公开必须退出 PIE/重启的边界，不靠 UI 隐藏重建成本。

**四、完整调试需要执行模型支持。** 当前暂停范围集中在顶层同步 void export。仅增加面板或 IDE 启动配置，不能支持跨 Guest 调用帧、非 void 返回、异步因果链与暂停期间 GC。P66 的帧/根合同应为 P68 预留稳定入口，P68 再交付真实调试工作流。

**五、托管对象成本需单独预算。** 当前受追踪对象经 Host ABI 访问，不能把早期标量微基准成绩外推为闭包密集玩法的速度。需要分别记录分配量、保活、回收暂停、Host crossing 和尾延迟，再决定优化对象布局或批处理。

代码与本地证据：[语言入口及明确拒绝](../../Tools/AvidScript.CSharpGuest/Lowering/CSharpGuestLowerer.cs)、[ref/out 边界](../../Tools/AvidScript.CSharpGuest/Lowering/CSharpOperationLowerer.cs)、[调试 Gate 范围](../Phase61/P61.E_Integration_Gate.md)、[当前实现边界](../../README.md#当前边界)、[闭包分配合同](P66.B_Closure_Allocation_Results.md)。

## “跨时代”应落实为怎样的体验

建议产品目标是：**开发者在运行中的游戏里修改玩法、理解异步与状态变化、验证结果，并安全地保留或撤回变更。** 第一阶段不另造语法，先让熟悉的 C# 表达可靠可用。

| 能力目标 | 用户可感知的结果 | 必须证明的边界 |
| --- | --- | --- |
| 运行中编辑 | 保存普通玩法修改后快速生效，尽量保留实例状态 | 哪类修改保持 PIE；迁移失败如何保留旧版本；网络两端如何兼容 |
| 有归属的异步 | 技能、UI 或 Actor 销毁后，关联任务与订阅自动结束 | 取消、异常、重入、await、reload 的组合测试 |
| 可解释执行 | 可从故障跳到源码、对象、await 来源与相关事件 | 内部函数、返回值、跨层调用栈和重载后映射真实可用 |
| 可复现验证 | 记录受控输入与事件，在测试世界重现问题 | 随机、时间、网络和外部副作用显式建模；不宣称整个 UE World 天然确定 |

最后一项属于后续探索，不加入当前 P66 完成声明。应先在小型受控 gameplay slice 验证成本，再决定是否扩展为产品方向。

## 推进优先级与验收

保留已经冻结的 P66–P69 顺序，不重新解释已完成批次：

1. **P66：完成日常语言语义。** 首先修复当前闭包编译失败，验证共享 cell、逃逸、嵌套、循环与回收；随后打通实例/委托/事件、跨 await 生命周期、集合和错误清理。测试同时包含真实 WASM 和与 .NET 参考结果的语义对照。
2. **P67：证明结构修改的编辑循环。** 先做 UE 原型，再决定实现；同时覆盖 Blueprint、GC、复制及 Cook，不能只测试裸脚本类。
3. **P68：补齐真实调试。** 内部函数、返回值、异步链、闭包、reload 后断点与变量，使用真实 IDE 工作流验收。
4. **P69：用真实 Windows 玩法决定成熟度。** 同一套技能、UI、存档和网络任务，比较首次实现、需求修改和故障定位；找未参与框架实现的开发者试用。

建议在实施前冻结以下对测指标。这些是拟定目标，不是已经达到的成绩：方法体小改动保存到生效 P95 ≤ 1 秒；同一完整任务迭代时间相对两种对照分别降低至少 30%；故障定位中位时间降低至少 50%；记录重启次数、手写桥接量与无法自然表达的用例。若固定机器和工程规模下目标不合理，应保留原结果并调整目标依据，不能删掉慢样本。

性能另设矩阵：统一语义、数据表示、优化和边界调用次数；分别比较 Editor 与 Shipping；AngelScript Shipping 包含其可用的转 C++ 路径，Puerts 包含 reflection/static 路径；报告 P50/P95、分配与回收。现有 README 记录的完整性能门槛为历史 16/18，通过部分不能改称全面领先；AngelScript 当前也没有已完成的同口径全矩阵。

成熟的最低判据是：普通开发任务无需为适配框架重写设计；诊断能解释失败；承诺的热重载和调试行为稳定；独立开发者能从安装走到 Windows Shipping。具备这些证据后，才评估是否在开发效率上领先。Android/Mac 继续暂缓，P65 剩余债务保持原状态。
