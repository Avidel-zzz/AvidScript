# Phase 54 执行分层与性能领先收尾报告

> 状态：完成；全面性能领先目标未达成，后续继续优化

## 阶段目标

Phase 54 建立可替换执行后端、WAMR AOT 闭环、多 execution lane benchmark 和 descriptor 驱动 crossing 快路径，并以正式数据判断 AvidScript 相对 Puerts 与 UnrealEngine-Angelscript 的性能位置。

## 批次状态

| 批次 | 状态 | 结果 |
| --- | --- | --- |
| P54.0 架构与计划 | 完成 | 多后端、JIT 对 JIT、Shipping AOT 与性能 Gate 已冻结 |
| P54.1 backend contract | 完成 | backend identity/capability/selection/artifact/factory/handle ownership 已实现 |
| P54.2 Wasmtime JIT backend | 完成 | Wasmtime 45.0.0 Cranelift JIT、动态 import、Runtime 生命周期、依赖锁与 WAMR parity lane 已闭环 |
| P54.3 JIT 对 JIT benchmark | 完成 | C# frontend 五通道 7500/7500 正确；同 WASM JIT 对照 600/600 正确，Wasmtime PC 候选 Gate 通过 |
| P54.4 crossing fast path | 完成 | descriptor shape typed thunk、关闭生产逐调用计时与 cached invocation context 已实现；正式矩阵 7500/7500 正确 |
| P54.5 qualified native direct | 完成 | 显式授权、加载期证明、`UFunction::Invoke` direct 与真实路径计数已闭环；标量 P50 较 P54.4 改善 69.31% |
| P54.6 generated-native / competitor gate | 完成 | Generated S1、typed Wasmtime host trampoline、Data-Oriented command buffer、六通道 gameplay/micro 与 12-kernel 集中 Gate 已闭环；3/12 性能门槛通过 |

## 性能结论

P54.3 已证明：在固定同一 WASM 整数 kernel、cached export、编译/实例化/导出查找均位于计时区外的边界下，Wasmtime Cranelift JIT 相对 Puerts V8 WebAssembly tiered JIT 的跨进程配对 P50 为 `0.9992x`，P95 为 `0.9835x`，均通过 `1.05x` PC 候选门槛。

P54.4 将 `scalar_add_int32` 从 751.40 ns/op 优化到 713.14 ns/op，并证明
`ProcessEvent` 内部 frame 仍是主要固定成本。P54.5 的 qualified native direct 将同一
workload 进一步降到 218.84 ns/op，相对 P54.4 改善 69.31%，相对同次 semantic
lane 改善 67.93%；正式样本记录 960 万次真实 direct hit、零 direct 回退。

P54.6 将物理 typed empty import 压到 P50 `4.5785 ns`、P95 `6.315 ns`。在 12 个
相同 WASM kernel 上，Wasmtime/V8 P50 几何均值比为 `0.9772`，赢得 `9/12` 个 P50
kernel；P95 几何均值比为 `1.0159`，只赢得 `4/12`，未通过全面领先门槛。

UE gameplay 组合路径已经取得正式领先：Small frame 的 Generated S1 为
`83.77 ns/op`，是最快 Puerts Static 的 `0.5943x`；Dense frame 的 Data-Oriented
路径为 `126.19 ns/op`，是最快 Puerts Static 的 `0.5488x`，两项 P95 与 `2 x MAD`
也均通过。`FVector` value 为 `199.97 ns/op`，本次快于 Puerts Static 的
`1053.81 ns/op`。

该结果仍不能解释为 AvidScript 已经全面领先 Puerts。Generated S1 标量为
`54.12 ns/op`，仍是 Puerts Static 的 `1.95x`；属性为 `197.09 ns/op`，空 callback
为 `385.86 ns/op`。集中 Gate 共 12 项，只通过 `FVector` 改善和两项 gameplay，
其余 9 项失败。Phase 54 完成的是现代多后端、生成绑定、数据导向与可信 benchmark
闭环，不是最终性能目标的终点。

## 验证证据

- P54.2：no-clean UBT 通过，聚焦 Automation 16/16 通过，独立审查 Accepted；
- P54.3 C# frontend：7500/7500 observation 正确，aggregate SHA-256 为 `595ce9cd9d70a3f12649fa54979c55510e8d48b917dd7cc4af22dccc31e3fde7`；
- P54.3 同 WASM：600/600 observation 正确，6 个唯一 PID，fallback 关闭，aggregate SHA-256 为 `157bbe394ea19b7c68e35d5b1f5fcf2b2bf14f2330e04a8e4d987c040579f1a6`；
- P54.4：UE5.8 no-clean build 与定向 Automation 通过，正式 7500/7500 observation 正确；
- P54.5：UE5.8 no-clean target build、qualified-direct 与 binding-slice Automation、prepare、五 lane correctness 和正式 7500/7500 observation 通过；
- P54.5 aggregate SHA-256 为 `3c34acfaab9400cd38013a078ece5fbab9aba8f53a611baef0958719f6814509`；
- P54.6：UE5.8 no-clean build、聚焦 Automation `16/16`、固定 .NET `193/193`、
  WAMR SIMD blocker probe、12-kernel controlled runtime、physical cost、micro 与
  gameplay 正式矩阵均通过正确性和 provenance 校验；
- P54.6 正式候选为 `246f9a9201a7a8b8d96ec98e6f7f7d7170d2a3d8`，tree 为
  `25bfd4f554c490f1017bafab4d37e8e617b50b7d`；controlled suite SHA-256 为
  `77bac14d24e1a164bb108194a42a45299fcdb540e428b6ebe705a195927a6bc0`，集中 Gate
  SHA-256 为 `239fed0d3d4df65a2ca23081781ae696f2f7ee3b1ce8f6dd7d6533a2647908e1`，
  compact evidence SHA-256 为
  `f3e643fcc4e86c44c32c91c0046f03c966ee30a1a43c436ec59d3f3bdabc99f5`。

## 剩余风险

- controlled-runtime P95 仍落后 V8 `1.59%`，且 P95 kernel win rate 只有 `33.3%`；
- Generated S1 标量、属性与 callback 仍落后 Puerts，物理 typed import 已足够低，
  下一阶段应聚焦 receiver、绑定调度、callback 生命周期与 UE 语义固定成本；
- Data-Oriented schema 1 只开放事务式 `SetI32`，`FVector` 写入、独立调用与对象赋值
  仍需完成 alias、异常顺序和 payload 合同后再开放；
- 完整普通 UFUNCTION 由 semantic reflection fallback 保证覆盖，Generated S1 只覆盖
  已生成的常用 shape，不能把性能快路径等同于完整 UE API 都已生成；
- WAMR AOT compiler 在 Win64 的 LLVM/toolchain 闭包尚未验证；
- AOT 对 Host crossing 的改善上限未知；
- UnrealEngine-Angelscript 依赖引擎源码修改，当前引擎尚不能作为同机对标候选；
- 移动端只完成架构约束，尚未运行设备 benchmark。
