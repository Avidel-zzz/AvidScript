# Phase 52 双向属性 Binding 收尾报告

> 状态：集中 Gate 待执行。P52.1 至 P52.4 已实现并有 focused 证据；P52.5 正在完成最终独立复审、全量验证、attestation 与 close。在这些步骤完成前不声明 Phase 52 已完成。

## 阶段目标

把 descriptor 驱动的 reflected property 从只读升级为显式授权、缓存执行、可回滚的 C# `get/set` 闭环，并建立后续容器、delegate 与通用成员写入共用的能力模型和事务基础。

## 已完成能力

- profile 显式声明属性读写意图，旧只读 profile 的 hash 与行为保持兼容；
- descriptor schema v8 发布独立 `property_get/property_set`、写策略和精确 BlueprintSetter 身份；
- C# renderer 生成自然属性，Roslyn 语义可达性按 getter/setter 精确裁剪；
- C# lowering 保证 receiver 先于右值求值，复合赋值不重复求值 receiver；
- package load 缓存 `FProperty*`、owner class 与可选 setter `UFunction*`，热路径不做名称查找；
- direct setter 支持 scalar、object handle 与现有 inline UStruct ABI；
- candidate transaction 首次写入捕获类型正确的属性快照，失败逆序回滚，UObject 快照跨 GC 保持强引用；
- 具有不可逆副作用的 BlueprintSetter 在 candidate `ProcessEvent` 前失败关闭；
- runtime slice 只发布 C# 最终实际使用的访问器，并保持 package/import provenance 精确匹配；
- 中文 `BidirectionalProperties` 样例通过真实 profile、Roslyn、Guest IR、WASM、WAMR 与 UE Actor 生命周期闭环；
- 描述符解析失败现在保留包头、类型索引、binding 索引和对象图错误源，不再退化为空诊断。

## 实际游戏逻辑

样例在 `BeginPlay` 写入 `AActor.CustomTimeDilation` 并设置 Actor scale，在 `Tick` 继续写属性、读取 `FVector` scale 后计算并写回，在 `EndPlay` 恢复状态。最终 focused 测试证明：

- production profile 构建执行 bootstrap/final 两轮；
- setter-only 源只在 runtime slice 中保留 setter；
- manifest、runtime package 和 WASM 共同加载；
- `BeginPlay/Tick/EndPlay` 均进入真实 UE Actor；
- scalar property set 与 `FVector` function ABI 都产生可观察结果。

## 独立复审修复

初轮独立复审发现 1 项 High 和 5 项 Medium，集中修复覆盖：

- C# 属性赋值求值顺序；
- Phase 51 旧 profile identity；
- BlueprintSetter descriptor identity；
- object property snapshot 的 GC root；
- getter/setter accessor 级可达性；
- benchmark 防编译器折叠；
- 真实 profile 示例的 build request、诊断源位置、`UE.Self` struct 赋值语义、受支持 FVector API 和 package manifest/descriptor 分层。

最终独立复审结论将在 `P52.5_Independent_Review.md` 中记录。

## 性能

最终 anti-fold focused benchmark：

| 路径 | P50 | P95 |
| --- | ---: | ---: |
| Native setter | 0.000000393 ms | 0.000000975 ms |
| Cached binding setter | 0.000196480 ms | 0.000212887 ms |
| WAMR setter | 0.001013475 ms | 0.001106841 ms |

11,776 次调用保持一次 import crossing，warm class lookup、reflected-name lookup 与重复 snapshot capture 为 0。P52.4 初步 native 数字可被编译器折叠，已由这份 checksum 消费、禁止关键内联的最终基线取代。

## 当前边界

Phase 52 不宣称覆盖任意 out/ref UStruct、`FHitResult`、容器、delegate、Cook/Shipping 和移动端。`K2_SetActorLocation` 等接口需要通用 out/ref ABI 后才能安全进入脚本表面；不会通过为单个 UE API 手写特例绕过。

## 最终 Gate

最终数字将在冻结候选完成集中 Gate 后写入。计划保持一次 no-clean UE5.8 Editor build、一次完整 `Automation RunTests AvidScript`，并串行执行五个 .NET host、format、PowerShell contracts、parser 与 architecture fixtures。

## 当前结论

**Phase 52 功能实现已经闭环，阶段状态仍为实施中。** 只有 freeze、完整 Gate、attest、close evidence 与安全推送全部完成后，本文才会更新为正式完成。
