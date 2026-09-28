# AvidScript 跨语言交付合同（Win64 先行）

状态：迭代规划，尚未冻结为 Phase 架构或产品能力。实施状态以 [P66 状态](../Phase66/Phase66_State.json)和 [P65 性能债务](../Phase65/Phase65_State.json)为准；Android/iOS 真机验收暂缓。

## 什么算完成

同一个 UE 项目内，开发者分别维护 C# 与第二语言源码。两种语言通过同一份接口定义生成类型化 SDK，运行在同一套 Session、能力授权和对象生命周期规则下，能双向调用、传递复合值、回调、等待和取消，并可在 Editor 和 Win64 Shipping 中打包。第二语言首选 Rust 作为验证前端；选它是实施假设，不等于当前支持 Rust。

WAMR 与 Wasmtime 是两个 **VM 后端**，不是两种脚本语言；外部 WASM 能加载、单个标量函数能调用，也不等于跨语言完成。AvidScript 目前仍是以 C# 为主的开发者预览。[当前路线图](AvidScript_Iteration_Roadmap.md)记录整体顺序。

要证明“比 Puerts 和 AngelScript 更灵活、易用且性能占优”，必须分别通过三类证据：

1. **能力：** 同一需求能够自然写成两种语言并完整运行，不能为演示删掉共享引用、UE Interface、ref/out、异步或 Blueprint 路径。
2. **开发：** 未参与实现的开发者按公开安装文档完成任务；记录首次实现、修改、排障、重启、手写桥接和失败尝试。
3. **运行：** 当前正式构建在相同 UE 语义、正确性、运行模式与采样协议下，对 Puerts 和 AngelScript 的关键负载都满足预先冻结的门槛。

任一类失败都不能用另一类的成绩替代。AngelScript 官方提供热重载、Blueprint 子类与 VS Code 调试；Puerts UE 提供 TypeScript 自动绑定、增量编译和静态绑定。[AngelScript 官方概览](https://angelscript.hazelight.se/)、[Puerts UE 入门](https://github.com/Tencent/puerts/blob/master/doc/unreal/en/getting_started.md)、[Puerts 静态绑定](https://puerts.github.io/en/docs/puerts/unreal/template_binding/)是对照功能的来源。官方资料不是本项目的竞品实测。

## 接口模型：先做一条纵向链

第一条链固定为 **C# 技能 → Rust 计算模块 → C# 回调 → UE Actor**。输入包含字符串、记录、数组、受检 Actor handle；Rust 返回计算结果并调用 C# 更新同一 Actor。回调后挂起一个任务，在等待中销毁 Actor；检查取消、错误来源和所有资源释放。再分别更新两个模块：兼容更新应继续服务新调用，接口不兼容或候选失败应保留旧版本。

公共接口定义至少需要以下字段；文件格式和编码版本在原型阶段冻结，不能用 C# Semantic JSON 伪装另一语言：

| 合同 | 必须明确的行为 |
| --- | --- |
| 模块身份 | `module_id`、`language_id`、编译器与 ABI 版本、接口哈希、产物哈希、源码映射身份；加载时验证待执行字节 |
| 值 | 固定宽度数值、UTF-8 字符串、记录、数组、可选值和结果值；长度、对齐、复制/借用、释放和上限写入版本化 codec |
| UE 类型与 API | 两种 SDK 复用同一 binding descriptor；代表性矩阵包含 `FText`、字符串数组、嵌套容器、`TSet`/`TMap`、soft/weak 引用、delegate `ref/out`、默认参数及 Interface/Blueprint 调用。尚不能表示的组合在生成时准确拒绝，不能运行时静默降级 |
| UE 对象 | 复用 ObjectRegistry 的 slot/generation handle；每次访问检查类型、World、线程、owner、代次和授权；不跨模块传裸 `UObject*` |
| 调用 | 稳定方法 ID、参数方向与 ref/out 别名、同步重入深度和调用预算；由 Host/Session 路由，不共享两种语言的堆指针 |
| 异步 | owner 关联的 task/future ID，完成、失败、取消的单一终态；Actor/World 销毁、重载和候选失败都能终结或明确保留等待者 |
| 错误 | 跨语言传结构化错误码、来源语言/模块、源码位置和调用链；Rust 错误不冒充 C# 异常对象，VM/Host 拒绝不被当作普通脚本异常 |
| 更新 | 正在执行的调用绑定旧代；新调用按兼容规则选新代；迁移提交原子化，失败时旧实例和任务仍有定义明确的状态 |

接口工具只从一份声明生成 C# 与 Rust SDK；业务代码不得手写每个 UFunction 的双份桥接。两侧产物经现有包验证、import allowlist、guest memory 和能力门禁进入生产 Host。若现有 binding descriptor 无法表示某个公共类型，先升级 descriptor/codec 合同和版本，再扩展语言 SDK；不复制一套 Rust 专用 Runtime。

## 独立原型的实施次序

P66.D 先提供完整技能的源码、调用图、对象/任务 owner 和不可撤销副作用清单。以下原型据此选接口，Rust 模块保留自己的编译器与来源身份；C# 和 Rust 可以使用不同 WASM 实例，但跨语言调用统一经 Host/Session 路由，不直接借用另一语言的堆对象或线性内存地址。原型结果在 P67 冻结类型表示之前评审，不作为 P66 的补充 Gate。

| 增量 | 交付与可证伪用例 |
| --- | --- |
| **接口定义与产物身份** | 冻结最小版本化接口定义，生成 C#、Rust 两侧 SDK 与模块声明；验证接口哈希、来源语言、编译器/ABI 版本、实际执行字节及 import 授权。字段重复、未知类型、签名不一致和产物替换在加载前拒绝 |
| **同步双向调用** | C# 技能传 UTF-8 字符串、记录、数组和 Actor handle 给 Rust；Rust 返回结果并回调 C# 修改同一 Actor。两侧检查值、对象身份、调用顺序、重入预算、ref/out 别名和失效 handle。值可以复制或在当前同步调用内限时借用；借用不能进入回调之外的持久状态或跨 `await` |
| **异步与故障** | 回调之后挂起，再销毁 Actor/World、取消任务或使任一模块失败；Session 只发布一次完成/失败/取消，释放两侧帧、根与等待者。错误保留语言、模块、源码位置和跨模块调用链；Host/VM 致命拒绝不伪装为脚本可捕获错误 |
| **更新与成本** | 在等待期间分别执行兼容更新、接口不兼容、损坏产物和候选执行失败；活动调用留在旧代，新调用只有在提交后进入新代。候选准备期间的 RPC/存档等不可撤销作用须拒绝或延至提交，不承诺任意外部副作用回滚。记录每次 crossing、复制字节、分配、P50/P95、峰值内存及零残留；两 VM 都完成正反例 |

各增量以原始 C#/Rust 源码、生成文件、规范包、机器可读结果和可重跑命令交付。只通过静态生成器、手写桥接、单向标量调用或测试专用 import，都不能进入下一增量。Win64 Shipping、干净工程安装和第二语言的 UE 反射类型仍由下表的产品化阶段验收。

## 交付次序与出口

| 交付 | 必须产出 | 可复现的出口证据 |
| --- | --- | --- |
| **P66.C：自然 C# 语义** | 原始业务源码的静态状态、await、异常、token 组合；真实 Actor/World 销毁、reload、`async void` 错误路由；默认 Build And Bind | 同源 .NET 对照、Semantic→IR→WASM、两 VM 的正常/取消/销毁结果及零残留；正式 Editor 入口不能依赖测试专用开关。已有 29 场景 174/174 是专项证据，不覆盖这些未验项 |
| **P66.D：单语言完整玩法** | 一段 C# 技能、UI、网络、存档流程在真实 Editor Play 连续运行；从这个流程抽取跨语言接口需求与成本基线 | 记录自然源码、缺失语义、构建/修改时间；沿已冻结 P66 Gate 收尾，不把第二语言实现追加为本 Phase 的完成条件 |
| **P66.D 后独立原型** | 接口定义、C#/Rust SDK 生成器与最小纵向链；在 P67 类型合同冻结前给出结论 | C#→Rust→C#→UE 通过生产 Host 的两 VM 正反例，包含复杂数据、回调、取消、回滚和来源诊断。失败先修公共 ABI，不绕过 Host |
| **P67：结构修改** | C# 定义的 Actor/Component/Subsystem 与 Blueprint 子类增字段/函数的迁移；接口版本升级策略 | 原始实例、CDO、GC、复制、Cook、失败回滚；明确哪些变化可在 PIE 中应用，哪些只需退出 PIE，哪些仍需重启 Editor。跨语言等待和旧接口调用不得悬空 |
| **P68：源码调试** | C# 与 Rust 的源码映射、内部函数帧、闭包/局部值、异步因果链；IDE 断点、步进和错误跳转 | 对同一玩法注入跨模块错误，开发者能从 UE 事件追到发起语言、回调与 await；暂停中销毁对象或重载无悬挂根；两代源码位置不混淆 |
| **P69 前：跨语言产品化** | 正式 SDK/包格式、兼容校验、诊断、Cook/Shipping、安装与升级路径；两语言的 UE API 类型矩阵和反射类型实现策略 | 两种源码和生成产物在干净 Win64 工程可重建；正常/不兼容/损坏/过期 handle 均有负例；Rust 侧能经同一生成类型壳实现 Actor/Component/Subsystem 的一个 Blueprint 子类可调用成员；独立开发者不修改插件 Runtime 即可新增模块 |
| **P69：真实 Windows 游戏与对测** | C#/Rust 共同组成技能、UI、网络和存档 Demo；长跑、热重载压力、发布包；Puerts/AngelScript 同需求对照 | 独立开发者从公开仓库安装到 Shipping；玩法正确性、异常/取消/内存、编辑耗时和正式性能矩阵同时达标。失败样本与原始结果保留 |

P67/P68 和第二语言正式实现的架构仍须按 Phase workflow 单独冻结。本表只规定跨阶段依赖，不变更 [P66 冻结架构](../Phase66/P66.0_Developer_Experience_Architecture.md)或已完成批次。第二语言原型开始前必须从 P66.D 的真实玩法抽取接口，不先发明与 UE 任务无关的通用 FFI。

## 三种比较如何执行

**功能。** 冻结一个 Windows 任务：按键启动技能，经接口调用另一个 Actor，异步加载，Timer 等待，事件闭包累计命中，更新 UI，服务器确认并保存。随后改伤害公式、增加字段、在 await 中销毁 Actor、注入跨模块错误。三套框架允许惯用写法，但输入、结果、错误与资源释放断言相同；对方不支持的语义按“不支持”记录，不替其改造需求。Puerts 的 UE 语言范围以[官方仓库](https://github.com/Tencent/puerts)为准；其 Unity Lua/Python 不计入 UE 对照。

**开发。** 先做小规模流程预演并冻结说明书与计时口径，再让至少 8 名未参与框架实现的开发者交叉执行任务，平衡框架顺序并给予等量熟悉时间。记录保存到生效 P50/P95、首次构建、完整任务迭代、定位错误、重启 Editor/PIE、手写桥接量及放弃任务。目标沿用[路线图](AvidScript_Iteration_Roadmap.md#用户能感知的领先)：方法体修改 P95 ≤ 1 秒，任务迭代时间相对两者各低至少 30%，故障定位中位数各低至少 50%；样本量、区间和失败尝试一同发布。

**性能。** 先冻结三套框架的同语义正确性，再跑同一最终 UE5.8 构建与 Win64 Editor/Shipping 配置，区分纯执行、UE crossing、回调/async、集合和完整技能帧。AvidScript 的主胜负矩阵使用 C# 玩法路径对照 Puerts TypeScript 与 AngelScript；C#/Rust 混合路径单列为跨语言收益与开销，不能用 Rust 计算速度替代脚本框架的主矩阵。沿用 [P65 正式协议](../Phase65/P65.D1_Performance_Leadership_Protocol.md)与原门槛；新跨语言负载单独升级协议版本，记录 C#↔Rust crossing 次数、复制字节、分配、P50/P95 和峰值内存。关键脚本 CPU 成本目标为 P50/P95 比两对照中更快者各低至少 20%，其余核心负载不出现超过 5% 且可重复的退化。AngelScript Shipping 包括可用的生成原生路径，Puerts 包括 reflection/static；安全检查、正确性和失败路径不得为计时关闭。

当前 [P65.D37](../Phase65/P65.D37_AngelScript_Same_Semantics.md)已验证 AngelScript 正确性，[P65.D38](../Phase65/P65.D38_AngelScript_Timing_Host.md)只得到它的 Editor VM 计时，[P65.D39](../Phase65/P65.D39_Isolated_Editor_Six_Lane_Preparation.md)只得到其他路径的单进程诊断；它们不能相除得出正式排名。下一性能动作是隔离 Editor 身份入口入库、冻结最终输入、同配置重跑三方五进程矩阵、补 AngelScript Shipping 路径并修复现有红项。现阶段不能宣称性能领先。

## 改变路线的条件

- 如果自然 C# 技能必须反复改写为测试专用形状才能编译，暂停扩张语法表，评估现有 Guest IR/自建运行时是否仍是正确执行路线；保留原始失败源码与诊断。
- 如果 C#↔Rust 的复杂值、owner 取消或回滚只能靠绕过 Session/Host 完成，先修改公共接口与生命周期合同，不将旁路放进 SDK。
- 如果结构更新无法安全迁移 Blueprint、GC 和复制状态，P67 须公开重启边界并审查类型表示，不把方法体重载称为结构热重载。
- 如果正式同语义性能门槛未过，继续定位执行层、边界与调度成本；不能用一条更快的微基准或旧候选成绩宣称全面领先。
