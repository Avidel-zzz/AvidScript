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
| P54.4 crossing fast path | 未开始 |  |
| P54.5 Shipping AOT/移动端 | 未开始 |  |
| P54.6 competitor gate | 未开始 |  |

## 性能结论

P54.3 已证明：在固定同一 WASM 整数 kernel、cached export、编译/实例化/导出查找均位于计时区外的边界下，Wasmtime Cranelift JIT 相对 Puerts V8 WebAssembly tiered JIT 的跨进程配对 P50 为 `0.9992x`，P95 为 `0.9835x`，均通过 `1.05x` PC 候选门槛。

该结论只证明当前固定执行 kernel 上两者基本同档，不能解释为 AvidScript 整体已经全面领先 Puerts。C# frontend 五通道正式矩阵仍显示 scalar、property、object 和 callback crossing 存在明显固定成本；P54.4 必须以 descriptor 驱动 typed thunk 与 cached call plan 继续降低这些成本。

## 验证证据

- P54.2：no-clean UBT 通过，聚焦 Automation 16/16 通过，独立审查 Accepted；
- P54.3 C# frontend：7500/7500 observation 正确，aggregate SHA-256 为 `595ce9cd9d70a3f12649fa54979c55510e8d48b917dd7cc4af22dccc31e3fde7`；
- P54.3 同 WASM：600/600 observation 正确，6 个唯一 PID，fallback 关闭，aggregate SHA-256 为 `157bbe394ea19b7c68e35d5b1f5fcf2b2bf14f2330e04a8e4d987c040579f1a6`；
- 同 WASM实现、原始结果、统计与 Gate 经独立复审 Accepted，无 Critical/Important。

## 剩余风险

- fixed kernel 每个 JIT sample 含 8,192,000 次内循环，短函数 crossing 成本被显著摊薄；
- C# frontend 五通道中的 scalar、property、object 和 callback crossing 仍明显慢于 Puerts，P54.4 typed thunk 尚未实现；
- WAMR AOT compiler 在 Win64 的 LLVM/toolchain 闭包尚未验证；
- AOT 对 Host crossing 的改善上限未知；
- UnrealEngine-Angelscript 依赖引擎源码修改，当前引擎尚不能作为同机对标候选；
- 移动端只完成架构约束，尚未运行设备 benchmark。
