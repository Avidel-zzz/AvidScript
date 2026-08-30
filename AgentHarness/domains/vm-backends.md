# VM Backend 策略

## 抽象

- `AvidScriptVM` 只依赖稳定 backend interface；WAMR、Wasmtime 或后续 backend 不泄漏实现类型到 Runtime/Bindings。
- 模块加载、实例化、调用、memory、trap、fuel、serialization 和 capability discovery 由 backend adapter 统一表达。
- WASM-first 不等于绑定单一 runtime。PC 可优先高性能 JIT/AOT，移动端按平台 JIT 限制提供 AOT/解释后端。

## 安全

- 被验证的 canonical bytes 必须就是被执行的 bytes；artifact identity、runtime version 和 feature set 绑定。
- host import 使用最小 capability surface，按 Session 注册；未知 import、版本或签名一律拒绝。
- VM 内存访问先验证范围、alignment 和生命周期；trap 后 Session 状态保持可诊断且不可误继续。

## 性能

- backend 比较必须统一 WASM 模块、优化级别、JIT/AOT 模式、warmup 和 Host crossing 次数。
- serialized/JIT cache key 包含 canonical module hash、backend、CPU features、runtime version 和 ABI。
- 优化先定位纯执行、调用桥、memory copy、codec、reflection 和 scheduling 各自占比，避免用总耗时猜根因。
