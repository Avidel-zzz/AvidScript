# Phase 54 执行分层与性能领先收尾报告

> 状态：进行中

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
| P54.6 generated-native / competitor gate | 未开始 | 以生成式 S1 binding、Wasmtime host trampoline 和 Puerts/Angelscript 集中 Gate 收束 Phase 54 |

## 性能结论

P54.3 已证明：在固定同一 WASM 整数 kernel、cached export、编译/实例化/导出查找均位于计时区外的边界下，Wasmtime Cranelift JIT 相对 Puerts V8 WebAssembly tiered JIT 的跨进程配对 P50 为 `0.9992x`，P95 为 `0.9835x`，均通过 `1.05x` PC 候选门槛。

P54.4 将 `scalar_add_int32` 从 751.40 ns/op 优化到 713.14 ns/op，并证明
`ProcessEvent` 内部 frame 仍是主要固定成本。P54.5 的 qualified native direct 将同一
workload 进一步降到 218.84 ns/op，相对 P54.4 改善 69.31%，相对同次 semantic
lane 改善 67.93%；正式样本记录 960 万次真实 direct hit、零 direct 回退。

该结果仍不能解释为 AvidScript 已经全面领先 Puerts。标量 direct 比 Puerts
Reflection 慢 1.92 倍，比 Puerts Static 慢 7.63 倍；当前胜利是 AvidScript 自身
crossing 架构的大幅改善，不是竞争框架总体排名逆转。

## 验证证据

- P54.2：no-clean UBT 通过，聚焦 Automation 16/16 通过，独立审查 Accepted；
- P54.3 C# frontend：7500/7500 observation 正确，aggregate SHA-256 为 `595ce9cd9d70a3f12649fa54979c55510e8d48b917dd7cc4af22dccc31e3fde7`；
- P54.3 同 WASM：600/600 observation 正确，6 个唯一 PID，fallback 关闭，aggregate SHA-256 为 `157bbe394ea19b7c68e35d5b1f5fcf2b2bf14f2330e04a8e4d987c040579f1a6`；
- P54.4：UE5.8 no-clean build 与定向 Automation 通过，正式 7500/7500 observation 正确；
- P54.5：UE5.8 no-clean target build、qualified-direct 与 binding-slice Automation、prepare、五 lane correctness 和正式 7500/7500 observation 通过；
- P54.5 aggregate SHA-256 为 `3c34acfaab9400cd38013a078ece5fbab9aba8f53a611baef0958719f6814509`。

## 剩余风险

- fixed kernel 每个 JIT sample 含 8,192,000 次内循环，短函数 crossing 成本被显著摊薄；
- qualified direct 当前只覆盖 `instance + 2 x value int32 -> int32`，属性、对象、向量、回调和绝大多数 UFUNCTION 仍走 semantic fallback；
- 标量 qualified direct 仍比 Puerts Reflection 慢 1.92 倍，host import、动态 ABI、receiver/ordinal 解析和 `FFrame` 仍需继续压缩；
- generated-native S1 binding 尚未实现，当前不能与 Puerts Static 做同层领先宣称；
- WAMR AOT compiler 在 Win64 的 LLVM/toolchain 闭包尚未验证；
- AOT 对 Host crossing 的改善上限未知；
- UnrealEngine-Angelscript 依赖引擎源码修改，当前引擎尚不能作为同机对标候选；
- 移动端只完成架构约束，尚未运行设备 benchmark。
