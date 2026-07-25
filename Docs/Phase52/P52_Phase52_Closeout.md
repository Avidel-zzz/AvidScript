# Phase 52 双向属性 Binding 收尾报告

> 状态：已通过最终 Gate 并完成 attestation。P52.1 至 P52.5 均已完成；本文与 `P52_Gate_Summary.json` 构成收尾提交，外部不可变 close evidence 由工作流 `close` 命令生成。

## 阶段成果

Phase 52 已把 descriptor 驱动的 reflected property 从只读升级为显式授权、缓存执行、可回滚的 C# `get/set` 闭环：

- profile 显式声明属性读写意图，旧只读 profile 的 hash 与行为保持兼容；
- descriptor schema v8 发布独立 `property_get/property_set`、写策略和精确 BlueprintSetter 身份；
- C# renderer 生成自然属性，Roslyn 语义可达性按 getter/setter 精确裁剪；
- C# lowering 保证 receiver 先于右值求值，复合赋值不重复求值 receiver；
- package load 一次性缓存 `FProperty*`、owner class 与可选 setter `UFunction*`，热路径不做名称查找；
- direct setter 支持 scalar、object handle 与现有 inline UStruct ABI；
- candidate transaction 首次写入捕获类型正确的属性快照，失败时逆序回滚，UObject 快照跨 GC 保持强引用；
- 具有不可逆副作用的 BlueprintSetter 在 candidate `ProcessEvent` 前失败关闭；
- runtime slice 只发布 C# 最终实际使用的访问器，并保持 package/import provenance 精确匹配；
- 描述符解析失败保留包头、类型索引、binding 索引和对象图错误源；
- 独立复审发现的问题全部关闭，终轮结论为 Critical 0、Important 0、Minor 0。

## 游戏逻辑闭环

中文版 `BidirectionalProperties` 样例已通过真实 profile、Roslyn、Guest IR、WASM、WAMR 与 UE Actor 生命周期：

- `BeginPlay` 写入 `AActor.CustomTimeDilation` 并设置 Actor scale；
- `Tick` 继续写属性、读取 `FVector` scale 后计算并写回；
- `EndPlay` 恢复状态；
- production profile 完成 bootstrap/final 两轮构建；
- setter-only 源在 runtime slice 中只保留 setter；
- manifest、descriptor、runtime package 与 WASM 共同加载；
- scalar property set 与 `FVector` function ABI 都产生可观察结果。

这证明当前 C# 脚本已经能在 UE 生命周期中读取和写入授权 reflected property，而不是只验证孤立 Host 函数。

## 架构边界

- Core 保持无 UE/WAMR 依赖；
- Bindings 负责 UE typed API、property plan 与 descriptor 调用，不依赖 WAMR；
- VM 只暴露 Core 级执行合同，不包含 gameplay 类型；
- Runtime 显式组合 Core、Bindings 与 VM，并持有 Session 和 candidate transaction 生命周期；
- Guest IR 保持语言中立，PowerShell 只负责编排和证据闭合；
- property、setter identity、调用能力与 runtime slice 均由 descriptor 生成链驱动，没有新增手写 UE API 白名单。

## 最终 Gate

| 验证面 | 最终结果 | 说明 |
| --- | --- | --- |
| .NET test hosts | 173 / 173 | 固定 SDK 8.0.416，五个共享 graph 宿主串行执行 |
| `dotnet format` | 5 / 5 | 精确候选，`--verify-no-changes --no-restore` |
| PowerShell contracts | 117 / 117 | 六个 Saved 依赖宿主 91/91，精确候选 PhaseWorkflow 26/26 |
| tracked PowerShell parser | 23 / 23 | 全部 tracked `.ps1` |
| frozen architecture fixtures | 17 / 17 | canonical、token、closure 与 path 正负例 |
| 主架构检查 | 通过 | 精确候选，架构输入与模块边界均 clean |
| UE5.8 no-clean UBT | 通过 | 5 个增量 action，`Result: Succeeded`，没有清理 Editor Target |
| full Automation | 276 / 276 | failed/not-run 0，Queue Empty、TestExit、RequestExit status 0、进程退出码 0 |
| 独立复审 | 0 findings | Critical 0、Important 0、Minor 0 |

外部包装器出现过三类非产品假阴性：变量插值语法错误在任何测试启动前被 parser 拦截；主 worktree 与冻结 worktree 的 CRLF/LF 差异被原始字节哈希误判，随后改为规范化内容身份；主架构脚本被错误交给 Windows PowerShell 5.1，随后使用规定的 PowerShell 7 重跑静态 Gate。被拒的 Attempt 日志均保留，最终 attestation 只引用通过日志。Gate 报告首次还因给非 Automation 检查附加扩展 `counts` 被 schema 拒绝，状态未改变；最终报告按既有 schema 重新生成并通过验证。

## 性能结果

同一份 276/276 完整 Automation 日志包含最终 anti-fold 属性性能采样：

| 路径 | P50 | P95 |
| --- | ---: | ---: |
| Native setter | 0.000000393 ms | 0.000000589 ms |
| Cached binding setter | 0.000195898 ms | 0.000206441 ms |
| WAMR setter | 0.000925778 ms | 0.000964450 ms |

11,776 次调用保持一次 import crossing，warm class lookup、reflected-name lookup 与重复 snapshot capture 均为 0，checksum 为 23.78。P52.4 的初步 native 数字可被编译器折叠，已由这份 checksum 消费、禁止关键内联的最终基线取代。

同轮历史性能面继续通过：组件 WASM cycle P50/P95 为 0.009725/0.012083 ms，每轮 4 次 import；typed checked cast P50/P95 为 0.000764/0.001237 ms，typed upcast Host import 为 0；对象 spawn/destroy 各为 1 次 import。上述数字是 UE5.8 Development Editor / NullRHI 下的 smoke benchmark，不替代 Shipping、移动端和长期回归基线。

## 当前边界

Phase 52 不宣称覆盖任意 out/ref UStruct、`FHitResult`、容器、delegate、Cook/Shipping 和移动端。`K2_SetActorLocation` 等接口需要通用 out/ref ABI 后才能安全进入脚本表面；不会通过为单个 UE API 手写特例绕过。

## Gate 身份

| 字段 | 值 |
| --- | --- |
| verified commit | `8f6766e920bb51545ea782f4010a728ff37e65c8` |
| verified tree | `37c1284060e8d089c29f2f25fefa328d55fe1b83` |
| verified state SHA-256 | `aeb32dcb9ac41201e01c9d776f3ea306e6290adcab516208cbe63e54dfee0064` |
| gate report | `C:\tmp\AvidScriptPhase52Gate-8f6766e-final\Phase52_Gate.json` |
| gate report SHA-256 | `3b36243a10ba1a97a4f2ba69c59b05ad9edfaff6b80d7a7449a386429d2b4609` |
| attestation commit | 本收尾提交 |
| close evidence | `C:\tmp\AvidScriptPhase52Gate-8f6766e-final\Phase52_Close.json` |

## 下一阶段

Phase 53 将建立通用 out/ref UStruct ABI：由 descriptor 发布参数方向与稳定 call-frame layout，C# renderer/lowering 支持 `out/ref`，Guest IR 与 WASM linear memory 承载可写槽位，Host 使用缓存 marshalling plan 执行并把结果写回。首个完整证明目标是让 `FHitResult` 与 `K2_SetActorLocation` 通过同一生成链进入 C# gameplay，不增加单 API 特例，并建立零热路径名称查找、可量化 crossing 与分配预算。

## 当前结论

**Phase 52 已完成。** C# 双向 reflected property、candidate 回滚、accessor 级 runtime slice、真实 UE 生命周期样例、完整 Gate、attestation 与 close evidence 形成了可复核闭环。
