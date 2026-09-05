# Phase 64 集成审查与集中修复

本轮只对 `e036d07..2010168` 做一次阶段级审查，发现六项问题后统一修复，不为每个修改重复复审。
集中修复的定点验证已通过，完整 Gate 尚未运行，不能使用 P63 的通过数字代替本阶段证据。

| 问题 | 影响与修复要求 |
| --- | --- |
| 正常 World 启动顺序 | Subsystem 早于 Actor BeginPlay；启动编排必须等待真实加载结果，再决定激活或整组回收 |
| reload 中的碰撞回调 | 候选执行及回滚产生的延迟事件不能发送给旧 Guest；候选队列须随提交或拒绝处置 |
| Android 报告身份 | Run ID 只代表新运行，不代表新脚本；同时核对 Runtime 实际 PackageId 与调用者预期值 |
| JSON 数字原始类型 | 数字字段先验证 `EJson::Number`，再验证整数、范围和有限值，拒绝布尔或字符串转换 |
| 样例重开计时器 | 保存并取消复活 timer，拒绝旧句柄回调；隐藏等待期不重复收集 |
| PowerShell 默认参数 | 顶层默认值不在 `PSBoundParameters` 中，包装器必须显式传递所有生效参数 |

定点探针又确认一项集成缺口：无头 Generated Type 发布会生成 schema v1 的
`wasmtime_precompiled` 描述，而 Editor 加载器仅接受 `wasmtime_jit`，导致后续 World 初始化报错。
Runtime Host 现已核对实际 AOT/policy 身份，不允许 JIT 描述覆盖预编译策略。
冷启动 Editor 不能复用另一进程的临时执行证明：构建器写入 `runtime_package_id`，启动模块比对
module、package、generation 与 type manifest 身份后，交由现有 Cook package loader 验证发布包。
陈旧或失配的 Cook 指针直接拒绝，未放宽底层执行证明。
新增启动测试也必须使用合法 `/Game` 临时 package；`/Temp` 会在 schema 层失败，无法验证预期加载回滚。

修正版样例拒绝复活期间重复收集后，首次玩法探针虽然送达 5 个事件，实际只完成 4 次收集。
探针原先按固定起点追赶事件，首帧延迟可能压缩实际间隔。现改为从实际派发时刻计算下一事件，
完成等待也从最后派发计算；重新核对最终位置和缩放，而非只接受通用探针的成功标记。

## 实现边界

- Component 管理 reload 期间的事件队列，Session 继续管理 Runtime 与 Host effect 事务，不新增旁路 VM 接口。
- Android 只加强报告和模块包身份合同；没有 SDK/APK/设备时仍返回未运行，不能计为真机通过。
- Startup 数字类型与激活顺序使用现有解析器、协调器和 World owner，避免再建一套生命周期。
- 样例只使用生成式 UE API 与既有 timer 能力，不增加专用 Host API。

## 集中修复验证

| 验证 | 结果 |
| --- | --- |
| UE5.8 no-clean Editor UBT | 通过；最终增量 4 actions，8.85 秒 |
| 聚焦 Automation | 发现、完成、通过均为 9，失败 0；测试与进程退出码均为 0 |
| Android runner 合同 | 24/24，覆盖包身份、旧报告、默认参数和设备边界 |
| 修正版 PickupRush Editor | 5/5 事件，零丢弃，胜利位置 `(600, 0, 300)`，缩放 `(2.5, 2.5, 2.5)`，可见且碰撞启用 |
| 无头 Generated Type | 5 个类型恢复 Win64 Development 发布；冷 Editor 加载及真实 World 用例通过 |

PickupRush 实际加载的包为 `42fd8cdd9d934524c7017f793c535ecec64f5ffe7d63117377d32a12a1806ff0`，
报告确认 `resolved_from_package=true`。运行 ID 为 `64b159aac83b4ae7a63e80cd22f7e014`。
日志保存在仓库外 P64Gate 证据目录：`integration-ubt-probe-spacing.log`、
`integration-focused-20260904-04.log`、`integration-pickuprush-20260904-02.log`。
玩法 JSON 位于项目 `Saved/AvidScript/ScenarioProbe/<run_id>.json`，不提交机器日志。

此前失败探针和构建仍保留：非法 fixture package/module ID、测试误用私有 API，以及 schema/AOT
冷启动问题均不能计为通过。它们属于 Critical 集成问题的定点修复，不以此冒充一次完整 Gate。

## 最终集中审查（2026-09-06）

冻结前对 `9e08cdc..e94599a` 的后续 P64.D 实现只执行一次阶段级集中审查，并结合完整 Phase
`e036d07..e94599a` 的格式、隐私和架构预检。审查覆盖 VM 导出条目所有权与 generation、typed Host
失败的一次性消费、反射参数帧析构、非 Self receiver capability/World 边界、GC 后 borrowed 句柄压缩、
委托回调来源、C# async 短路重写、UI 存档对象所有权、Shipping package/receipt 身份和验证报告边界。
未发现新的 Blocker/Critical；已知人工与移动端缺口均已显式转入 P65，未冒充通过。

- 完整 Phase 共 200 个文件、23,352 行新增、767 行删除；`git diff --check` 通过。
- 仅扫描新增行的本机绝对路径、私钥、API key、access token、client secret 与 password 模式，
  23,352 行中命中 0。
- detached clean worktree 在 commit `e94599ad33aaf1d8bfb1c6e4761d5ceb9640c32a`、tree
  `91c361ff20585484a1d919c3fd78e2d2687fa235` 上执行架构检查通过；主工作树唯一偏差仍是已登记保护文件。
- D03 的自动 UI、双进程存档、正文/存取重载、网络和一小时 World 证据已闭环；D01、D02、D09
  分别保留 Android/设备、人工玩法和 Shipping UI 人工验收。

## 阶段 Gate 待办

冻结候选后统一运行完整 AvidScript Automation、固定 .NET、PowerShell 合同、UE5.8 no-clean Editor UBT
与干净架构 Gate。阶段 Gate 只验证冻结提交，不读取主工作树三项保护改动。

## 防复发

生命周期测试至少覆盖一个真实引擎启动顺序；跨执行边界的队列必须参与所属事务；
运行证据同时绑定 run、module 与 package 身份，并检查实际游戏状态；时间间隔不能被首帧追赶压缩。
通用规则已写入 Harness lessons，按需检索，不扩张根 AGENTS。
路径检索若未命中，应返回 `rg --files` 索引重新定位，不从类名猜测源文件所在目录。
