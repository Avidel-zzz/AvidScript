# Phase 50 验收收尾骨架

> 状态：预创建骨架，尚未进入正式收尾。本文不是 Gate 证据，也不代表 Phase 50 已完成。

## 阶段目标

Phase 50 的目标是为项目自定义 UE 类型建立 typed C# 到 WASM 再到 UE 的闭环：生成 typed `UE.Self`、typed class reference、typed `SpawnActor`、零 crossing upcast、一次 crossing checked downcast，以及自定义 native `UFUNCTION` 的 descriptor 驱动调用。

## 验收映射

| 验收项 | 实现范围 | 代码/审查线索 | 最终 Gate 证据 | 状态 |
| --- | --- | --- | --- | --- |
| typed `UE.Self` | Task 1、4、7 | `507e80a`、`7dc47d9`、`32dfdd3`；Task 4/7 review | 待填不可变 Gate evidence | Pending |
| descriptor v6 类型图与 identity | Task 2、3 | `c76a32f`、`91fd8fe`；Task 2/3 review | 待填不可变 Gate evidence | Pending |
| typed Blueprint Spawn | Task 4、9、10 | `7dc47d9`、`dd7cef0`；Task 9 review | 待填集成 Automation evidence | Pending |
| 自定义 native `UFUNCTION` 闭环 | Task 9、10 | `dd7cef0`、`0572d75`；Task 9 review | 待填真实 C#/WASM/WAMR/UE evidence | Pending |
| upcast 为零 Host crossing | Task 4、5、10 | `7dc47d9`、`8ac988b`；Task 5 review | 待填 Guest IR/WASM import evidence | Pending |
| checked downcast 为一次 Host crossing | Task 6、7、10 | `a92b895`、`c94861b`；Task 6/7 review | 待填 WAMR success/mismatch evidence | Pending |
| wrong owner 在 BeginPlay 前拒绝 | Task 8、10 | `9a9547e`；Task 8 review | 待填 Automation callback evidence | Pending |
| 拒绝 reload 后旧 Runtime 保活 | Task 8、10 | `9a9547e`；Task 8 review | 待填 Automation scheduler/Tick evidence | Pending |
| warm path 无 class load/path lookup | Task 3、6、11 | `1971539`、`0724a7b`；Task 6/11 review | 待填 benchmark 采样与复核 | Pending |
| 无项目专用 wrapper | Task 2、9、12 | `Build/CheckAvidScriptArchitecture.ps1` | 待填 architecture gate output | Pending |
| 中文样例、性能说明与收尾文档 | Task 10、11、12 | `0724a7b`；Task 11 review | 待填文档审查和 Gate evidence | Pending |

## 集中验证清单

以下项目在创建本骨架时均**未执行**，不得由本文推导为通过：

| 验证 | 计划命令/范围 | 当前状态 | 不可变证据 |
| --- | --- | --- | --- |
| PowerShell parser | 所有 tracked `.ps1` 经过 `Parser.ParseFile` | Pending | 待 Gate 写入 |
| .NET 合同与格式 | 固定 SDK `8.0.416`，共享 project graph 串行执行 | Pending | 待 Gate 写入 |
| 架构门禁 | `Build/CheckAvidScriptArchitecture.ps1` | Pending，本次仅执行脚本自检 | 待 Gate 写入 |
| UE5.8 no-clean UBT | 最终四模块 scoped build | Pending，未执行 | 待 Gate 写入 |
| 完整 Automation | `Automation RunTests AvidScript` | Pending，未执行 | 待 Gate 写入 |
| 性能 benchmark | typed dispatch 同机采样、回退判定 | Pending，未执行 | 待 Gate 写入 |
| 独立代码审查 | Gate 前针对最终候选树复审 | Pending | 待 Gate 写入 |

## 收尾前置条件

1. P50.1 至 P50.5 必须通过 `Build/InvokePhaseWorkflow.ps1` 依次完成，并保持状态机为 `implementing`，直至最终候选可冻结。
2. 所有 Blocker、Critical、Important 发现必须修复或按 workflow 记录准确 debt；不得用本文替代 debt 证据。
3. 最终候选提交必须先冻结，再执行一次集中 Gate，随后以匹配 tree/hash 的不可变报告 attest。
4. 只有 Gate、attest 与 close evidence 全部存在且 workflow 报告 `Stage: closed` 后，本文才可改写为正式 closeout。

## 最终结论

**Pending。** 本文件只提供验收映射与证据落点，当前不声明功能完成、构建通过、Automation 通过、benchmark 成功或 Phase 已关闭。
