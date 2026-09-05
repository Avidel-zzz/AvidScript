# Phase 64 收尾记录

状态：P64.A-D 实施完成，待集中审查、冻结与完整 Gate

## 目标交付

- 数据驱动的 Startup Scenario 与 Runtime 挂载层。
- 可直接构建和启动的 C# PC 游戏纵向切片。
- Android arm64 UBT、包身份和可选 ADB 设备运行 Gate。
- 集中验证、机器可读证据与未执行人工验收边界。

## 已验证基线

候选 `9e08cdc` 的技术回归：Automation 439/439（Queue Empty、TestExit 和进程退出 0）、
.NET 284/284、10 组 PowerShell 合同、干净候选架构检查及 UE5.8 no-clean Editor UBT 通过。

修正版 PickupRush 的 Development/Shipping 包均通过 5/5 事件与胜利状态；回执分别 21/21、19/19。
两种配置 canonical WASM 一致，实际运行的 PackageId 分别与各自发布结果一致。
机器报告保存在仓库外 P64Gate 的 `9e08cdc-technical-evidence.json` 与两份
`9e08cdc-package-<configuration>-project.json`；它们不是正式 Phase close 证明。

打包后已恢复 Win64 Development Generated Type 指针和 canonical Editor target，三份 BuildId 一致。

## UI 与存档小节

`UiSaveDemo` 已实现 C# 驱动 UMG 按钮、计分与 SaveGame。最终 run
`b41db4a6bbef42b68294c7ec882a3a6f` 在四个独立 Editor 游戏进程中通过保存、重启读回、缺档和 GC，
13/13 动作、8 次脚本事件、零丢弃回调；读取/GC 前后存档哈希相同。
同批 Binding/UI 资产专项 18/18、UI runner 合同 53/53 通过；这些不扩充上面的历史全量基线。
参见 [UI 与存档集成](P64.D_UI_Save_Integration.md)及[C# 使用说明](../../Samples/CSharp/UiSaveDemo/README.md)。

后续[异常流程小节](P64.D_UI_Save_Edges.md)已通过五进程 31/31 动作和 runner 79/79：覆盖错误类型、
负数/越界分数、空文件、Reset、写锁下保存失败、组件 EndPlay 与迟到事件。四次失败 Load 保留原对象，
退出后 Session/授权和按钮订阅已释放。run 为 `62bc62bd35a040eda72de5725073644b`，不代替 World/长稳验收。

## 后续专项

UI [正文重载小节](P64.D_UI_Reload.md)已通过 20 轮、84/84 动作、99/99 runner 合同和 11/11
生命周期专项。复用 NextTickAsync 在提交后初始化 UI，分数迁移、失败候选回滚及退出隔离通过；
实测 Wasmtime 45 JIT，62 个资源快照无相对基线增长。异步版本原有存档五进程复验也通过。

[async 短路修复](P64.D_Async_Short_Circuit.md)已通过 Guest 140/140 与 Semantic 98/98；
13 个 Guest IR 执行场景和 WASM 编译验证不替代真实 WASM 执行，也不并入历史完整技术基线。

架构 v8 的[存取穿插重载小节](P64.D_Save_Reload_Ownership.md)已通过 20 轮、165/165 动作和
20 次真实 GC；旧存档对象全部回收，103 个快照的 owned 为 0、borrowed 为 7～8。Runtime 仅在
GC 完成后清理失效 borrowed lease，活跃调用与候选准备期延迟；生命周期专项 18/18、runner 合同
118/118、原有纯 UI 重载 84/84 通过，不扩充历史全量基线。

架构 v9 的 [World 连续运行](P64.D_World_Soak.md)已通过 3601.889 秒、877 次地图重建、4386/4386 动作。
旧 World 及四类关联对象逐轮回收，UObject 数恒为 52523，活跃 Session 与订阅有界；最终新实例读回 877 分。
World 合同 46/46、原 UI 合同 118/118、原存档五进程 31/31 动作通过。进程内存相对第三轮预热基线增长，
已登记 `P64-D07-WorldSoakMemoryAttribution`，不宣称无泄漏或完整长稳 Gate 已通过。

架构 v10 的[包内 UI 小节](P64.D_Packaged_UI.md)已在 Development/Shipping 归档 Game 中各完成两个进程
5/5 写入、2/2 读取动作，回执分别 28/28、25/25，实际执行 Wasmtime 45 AOT。独立验证插件不进入主 Runtime
依赖方向；Component 只读诊断专项及既有回归 10/10 通过。2026-09-04 用户反馈首轮真实界面、
按钮及保存/Reset/Load 操作无问题；2026-09-05 同一 UserRoot 的第二个 Development Game 进程人工读档
也反馈无问题，正常退出且无 AvidScript/Fatal 错误。Shipping 视觉仍待验收，不关闭 D03。
Shipping 通过隔离 UserDir 的标准 Engine.ini 选择样例地图；不修改工程/包默认配置或引擎 Shipping 宏。
打包后 Generated Type 已恢复为 Development，canonical Editor no-clean 恢复仅执行 1 项元数据动作，
10.51 秒通过，引擎/工程/插件三份 Editor 模块 BuildId 一致。

架构 v11 的[内存归因小节](P64.D_Memory_Attribution.md)新增 backend 生命周期、两张缓存表和探针 JSON
占用的冷路径快照。100 次切图、501/501 动作、VM 诊断 6/6、World 合同 53/53 通过，no-clean UBT
22 动作、39.42 秒通过。后端存活数恒为 2，缓存 entry/bytes 为 0；第 50～100 轮进程提交量仍增加
5.188 MiB，JSON 估算增加 1.075 MiB，D07 保持 Fixing，继续测量 UE Trace、分配器与原生资源。
本轮不是完整长稳或性能 Gate，不扩充历史全量基线。

架构 v12 在同一[内存归因小节](P64.D_Memory_Attribution.md)补充 Editor 私有 Trace/FName 与显式 LLM
快照。默认 3 次切图、16/16 动作通过；LLM 运行 413.850 秒、100 次切图、501/501 动作通过。
Trace 内存与 FName 容量全程不增长；第 50～100 轮进程提交量增 3.109 MiB，LLM Platform 仅增
0.125 MiB。no-clean UBT 4 动作、15.93 秒、World 合同 178/178 通过；继续定位原生堆/JIT 分配与分配器保留空间，
不把 LLM 诊断开销当成性能退化，也不把已跟踪总量平稳当成无泄漏证明，D07 保持 Fixing。

架构 v13 的[原生分配追踪](P64.D_Native_Allocation_Tracing.md)新增可选 Memory Trace 与逐轮 GC 书签。
真实 50 次切图、251/251 动作、World 合同 188/188 通过；Insights 四组增长/释放查询完成。
下一步优先检查反射 FString 参数帧每轮 7 项/208 字节保留，以及登记为 D08 的 backend 旧导出/身份
无界保留；Blueprint 引脚延迟删除与编辑器工具开销另行区分。未把原始预留空间、符号不全或工具
exit 0 当成泄漏/无泄漏证明，不关闭 D07。

架构 v14 的[调用生命周期修复](P64.D_Invocation_Lifetime.md)已修复原生 UFunction 参数帧可能漏析构，
并把 Wasmtime 重载导出条目改为调用者按需持有，不再由 backend 永久保存全部历史身份/条目。
no-clean 构建 22+4 动作通过，Binding 1/1、Wasmtime 14/14 通过；D08 已关闭。修复后 50 轮 trace
四组 CSV 完整导出，`SetUtf8Value` 在 3→50 与 25→50 两个窗口均为 0 项/0 字节，对照修复前
329 项/9776 字节与 175 项/5200 字节，已知参数帧泄漏已消除。第二次一小时运行 3601.003 秒、
877 次切图、4386/4386 动作通过，Session/backend live/UObject/Trace/FName 均有界；探针 JSON
估算保留 24,629,664 字节，整个 Editor 进程仍增长 Physical 322.965 MiB、Virtual 82.109 MiB。
因此 D07 仍为 Fixing，不把具体产品修复扩大表述为整个进程无泄漏。

架构 v15 的[包内 World 长稳](P64.D_Packaged_World_Soak.md)把同一玩法迁入有界 observer 的真实
Development/Shipping Game。Development 同一进程运行 3602.566 秒，完成 1173/1173 次切图、
5866/5866 动作与 1173 次 cleanup/GC；UObject 恒 35,580、Session 恒 1、backend live 恒 2、VM cache
恒 0。Physical 在第 50 轮一次性增加约 73 MiB 后平台化，第三轮到终点为 +76.16 MiB；Virtual 为
+4.25 MiB。Shipping fresh receipt 25/25，实际 3 轮、16/16 动作和最终新 World 读回通过。产品层
参数帧与 backend 历史保留已修复，Editor/全历史探针开销、分配器驻留和包内资源已完成分层，D07
据此标记 Verified；该结论不等于整个进程零增长。

当前候选 `a7a9226` 复跑 RuntimeComponent 网络闭环：dedicated server + 2 clients 与 listen server +
1 remote client 两种拓扑 **2/2** 通过，共 5 个真实 UE 进程；Server RPC、脚本 handler、复制属性、
RepNotify 与客户端确认均完成，Runtime 保持加载且无脚本错误。run
`20260904T222449915Z_39344_6514b6d4` 实际编排约 103.835 秒，聚合 SHA-256 为
`cf98b6c6fc85a75f8a51b2cec4768c4195d6a3b3bf446d240c94f1f586837085`。这是当前提交的有界网络回归，
不宣称一小时网络长稳或替代物理输入验收。

## 完整 Automation 隔离恢复

首次完整 Gate 候选 `cb808b9` 实际执行 461 项、451 项成功、10 项失败，因此该候选保持失败且不用于
attestation。修复批使未安装 package 的自动 Subsystem 静默休眠，限定 GameplayEvent UObject handle
只在回调窗口授权，并按 Session fault 合同回收其拥有的 Actor；同时新增固定 SDK 的 Release/Debug
fixture 准备入口。UeTypeGenerator 5/5、no-clean UBT 14/14 actions 和聚焦 Automation 10/10 通过。
详见[完整 Automation 隔离恢复](P64.D_Full_Automation_Recovery.md)。正式全量数字等待新冻结候选 Gate，
不沿用本段聚焦结果或旧候选的部分成功数。

## 保留边界

P64.D 的自动 UI、跨进程存档、真实样例重载、网络闭环与包内一小时 World 长稳均已有独立时长和机器可读证据。
Shipping 人工 UI 轮次因用户要求不中断自动推进而明确记为未执行，转入 `P64-D09-ManualUiShippingVisual`；
Android toolchain/UBT/APK 继续由 `P64-D01-AndroidDevice` 转入 P65。人工游玩和真实设备验收独立保留。
P58 类型、iOS、发布工程及性能领先等总目标缺口不会随本阶段编号自动关闭。

## 流程修正

- 阶段静态检查应覆盖完整阶段 Diff，而非只检查最后一个提交；四处多余 EOF 空行已集中整理。
  包内 UI 小节另在暂存检查发现公共脚本 EOF 空行；新增文件必须纳入 `git diff --cached --check`，不能只依赖未暂存 Diff。
- 包归档遵守 ProjectRoot 边界，放入项目 Saved；仓库外临时目录用于报告，不能直接用作 ArchiveRoot。
- Shipping 剥离 source diagnostics 是预期行为，通过 canonical WASM、发布包与回执绑定身份，不为测试重新暴露诊断。
- Generated Type pointer 不含平台配置，须关联 catalog 的 module/package variant，不从相邻 JSON 猜测字段。
- Shipping BuildCookRun 前须通过受控 HeadlessRelease 发布当前 Generated Type Shipping 变体，并验证
  pointer 在 catalog 中唯一匹配；验证结束恢复已备份的 Development pointer，不能等 UAT 完成后才发现身份漂移。
- Automation 使用实际 Queue Empty 与 TestExit 日志，不从提前 Quit 的退出码推断队列完成。
- 内存归因过程中发生一次临时 PowerShell 汇总语法错误：`foreach` 语句的结果应先赋给变量再进管道；
  子代理工具也不能跨版本猜参数名。本批均在读取/协调层修正，没有改变被测候选或重跑 UE。
- 引擎字段校验矩阵不逐项创建完整文件系统 fixture；纯报告校验直接调用生产验证函数，保留必要
  端到端路径。中断且无结束回执的合同运行不能计为通过，测试组织调整不触发无关 UE 重跑。
- GUI 子系统工具必须由单一受控进程包装等待退出；直接 `& UnrealInsights.exe` 可能在 GUI 子进程
  仍工作时让 PowerShell 返回，随后重复启动会造成重叠分析和缺失 CSV。只重试离线分析，不重采 trace。
- 长任务成功后只读取报告 schema 中存在的字段；用于展示的尾部表达式失败不能覆盖已持久化的原生
  成功证据，也不应触发昂贵重跑。先校验报告、日志关闭和哈希，再单独修正包装层。
- 重放旧的 UI reload 命令前必须从当前 catalog/runtime snapshot 取得 PackageId，并核对启动正文 SHA
  与 baseline 制品；不能照抄旧文档中的身份。身份或正文漂移时探针应在零轮 fail-closed，随后重新生成
  同源 C# 制品，而不是替换 expected 值后反复试跑。
- 临时 PowerShell 证据命令也必须使用明确 token 边界：变量后接冒号写成 `${id}:`，Git 范围先构造
  `$range = "${base}..HEAD"` 再传给 git；删除临时 worktree 前以实际命名根（本轮为 `C:\tmp`）核对
  绝对路径，不能假定它一定属于 `[IO.Path]::GetTempPath()`。
- 首轮完整 Gate 在 clean detached UBT 暴露 ignored `AvidScriptGeneratedTypes.h` 硬依赖并正确失败。
  项目生成头/源必须成对启用；完全缺失是合法首次安装，部分存在则 fail-closed。修复后无生成产物
  39/39 actions、已有生成产物 8/8 actions 均通过；两个尚不存在的路径也必须登记为 UBT 外部依赖，
  否则 clean makefile 无法观察半份生成文件首次出现。登记后头文件单独出现会以 exit 8 拒绝。详见
  [干净安装生成模块修复](P64.D_Clean_Checkout_Build.md)，但仍需对新冻结提交重跑完整 Gate。
- 汇总 .NET 测试时不能用子进程尾部的 `5/5` 代替顶层套件计数；离线解析器必须精确匹配 suite 名称，
  本轮纠正后六套实际为 300/300，未因此重跑测试。PowerShell `foreach` 输出进管道前先物化为数组；
  Windows 下 `rg` 不接收 shell 风格路径通配符，应传目录并使用 `-g '*.md'`。不同证据域不通过命令
  分隔符塞进同一临时命令，避免退出码和输出归属混淆。
