# Phase 55 预绑定调用边界收尾报告

> 状态：实现完成；全面性能领先目标未达成，剩余工作转入 Phase 56

## 阶段目标

在不削弱 UE 语义、对象生命周期和热重载安全的前提下，压缩 Generated S1 scalar、
property 与 Wasmtime lifecycle callback 的固定成本，并通过同机正式矩阵判断是否
领先 Puerts Static。

## 批次状态

| 批次 | 状态 | 结果 |
| --- | --- | --- |
| P55.0 | 完成 | 架构、计划、性能 Gate 与 workflow 状态冻结 |
| P55.1 | 完成 | prepared host call cell、registry revision 与 revision-aware Self |
| P55.2 | 完成 | split int32 property ABI、生成桥接与 data lane 兼容 |
| P55.3 | 完成 | verified unchecked Wasmtime export 与 lifecycle 结果策略 |
| P55.4 | 完成 | UE5.8 构建、312 项 Automation、三 profile、正式矩阵与债务收口 |

## 正式结论

Phase 55 的候选 commit 为 `ae27606c0f763480c50ff644bd100be62342eb50`，tree 为
`5b603a2b4fa8101d9fcbfd1d64b6c8d897ed4b64`。正式 Gameplay 与 micro 均使用
5 个独立进程、每进程 5 个 warmup 和 30 个 timed sample，正确性失败为零。

本阶段证明了预绑定边界架构有效：

- property 从 Phase 54 的 `197.09 ns` 降到 `104.75 ns`，改善 `46.85%`；
- callback 从 `385.86 ns` 降到 `212.23 ns`，改善 `45.00%`；
- `FVector` 从 `199.97 ns` 降到 `58.83 ns`，通过阶段改善 Gate；
- Small gameplay 为 `87.18 ns`，是 Puerts Static 的 `0.629x`；
- Dense gameplay 为 `129.52 ns`，是 Puerts Static 的 `0.546x`。

两个 gameplay workload 的 P95 与 `2 x MAD` 分离也同时通过。当前已经能在代表性
组合游戏逻辑中稳定领先 Puerts，但不能宣称全面性能领先：Generated S1 scalar
仍为 Puerts Static 的 `1.67x`，property 未低于 `50 ns`，callback 仍为 Puerts
最快对照的 `1.45x`。集中 12 项 Gate 通过 3 项、失败 9 项。

相同 WASM 的执行层对照中，Wasmtime/V8 P50 几何均值比为 `0.9770`，P95 为
`1.0108`；P50 赢得 `9/12` kernel，P95 赢得 `6/12`。Wasmtime 平均性能接近 V8，
但尚未达到绝对领先和 tail latency 目标。

## 已交付能力

- 加载期验证、热路径消费的 prepared host call cell；
- 对象注册表 revision 与热重载安全的 Self 缓存；
- 独立 int32 getter/setter ABI，以及 semantic、generated、data-oriented 三路径兼容；
- Wasmtime lifecycle export 的已验证 unchecked typed call；
- BeginPlay、Tick、EndPlay 的低复制结果策略与完整失败诊断；
- C# 生成 profile 到 WASM、Generated C++ binding、UE runtime 的真实闭环；
- 普通 `UFUNCTION` 的 semantic reflection fallback，性能 shape 无需逐个手写 API。

## 转入 Phase 56

Phase 56 必须继续解决两组 Important 债务：

1. 融合 receiver、参数、返回和策略的 call frame，继续压缩 scalar、property 与
   callback，并以 Puerts 最快路径和绝对 ns 双门槛验收；
2. 审计 Wasmtime Cranelift 配置及 tail latency，将相同 WASM 的 P95 几何均值和
   kernel win rate 推入真正领先区间，同时提高完整 crossing 成本解释率。

不采用逐个手写 UE API 的方式换取局部数字；完整覆盖继续由描述符生成和反射 fallback
承担。

## 验证证据

- UE5.8 no-clean `AvidTPSTemplateEditor Win64 Development` 构建通过；
- Automation `312/312` 通过；
- semantic、generated S1、data-oriented 三个真实 C# profile 加载与正确性通过；
- controlled runtime、physical crossing、micro、gameplay 的 provenance 和正确性通过；
- compact evidence：
  `Docs/Phase55/P55.4_Bound_Crossing_Benchmark_Evidence.json`；
- 实现报告：
  `Docs/Phase55/P55.4_Bound_Crossing_Implementation_Report.md`。
