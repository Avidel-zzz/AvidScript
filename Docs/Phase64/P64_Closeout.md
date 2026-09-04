# Phase 64 收尾记录

状态：实施中，原始目标尚未完成

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
按钮及保存/Reset/Load 操作无问题；人工跨重启读档、Shipping 视觉仍待验收，不关闭 D03。
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

## 仍需完成

P64.D 依架构 v13 继续补齐内存归因与保留问题修复、UI 人工重启读档与 Shipping 视觉、网络/重载持续时长，以及 Android toolchain/UBT/APK。
人工游玩和真实设备验收独立保留。P58 类型、iOS、发布工程及性能领先等总目标缺口不会随本阶段编号自动关闭。

## 流程修正

- 阶段静态检查应覆盖完整阶段 Diff，而非只检查最后一个提交；四处多余 EOF 空行已集中整理。
  包内 UI 小节另在暂存检查发现公共脚本 EOF 空行；新增文件必须纳入 `git diff --cached --check`，不能只依赖未暂存 Diff。
- 包归档遵守 ProjectRoot 边界，放入项目 Saved；仓库外临时目录用于报告，不能直接用作 ArchiveRoot。
- Shipping 剥离 source diagnostics 是预期行为，通过 canonical WASM、发布包与回执绑定身份，不为测试重新暴露诊断。
- Generated Type pointer 不含平台配置，须关联 catalog 的 module/package variant，不从相邻 JSON 猜测字段。
- Automation 使用实际 Queue Empty 与 TestExit 日志，不从提前 Quit 的退出码推断队列完成。
- 内存归因过程中发生一次临时 PowerShell 汇总语法错误：`foreach` 语句的结果应先赋给变量再进管道；
  子代理工具也不能跨版本猜参数名。本批均在读取/协调层修正，没有改变被测候选或重跑 UE。
- 引擎字段校验矩阵不逐项创建完整文件系统 fixture；纯报告校验直接调用生产验证函数，保留必要
  端到端路径。中断且无结束回执的合同运行不能计为通过，测试组织调整不触发无关 UE 重跑。
