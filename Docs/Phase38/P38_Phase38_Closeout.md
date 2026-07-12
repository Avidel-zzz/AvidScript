# Phase 38 Gameplay 闭环收尾

## 结论

Phase 38 已完成从 UE 事件到 C# WASM 游戏逻辑再回写 UE Actor 的完整闭环，并把高频 Transform 读取接入真实批量 WASM ABI。

本阶段不是通过逐个手写 callback export 或 UE API 完成，而是建立两条可继续扩展的稳定路径：

1. 高频数据走 BindingId + guest memory view + batch import。
2. gameplay callbacks 走统一 event Schema + 唯一 `avid_on_gameplay_event` + 语言侧生成 dispatcher。

这两条路径是 Phase 39 正式语言前端和 Phase 42 UE Reflection Binding Generator 的底座。

## 小组完成情况

| 小组 | 内容 | Commit |
| --- | --- | --- |
| P38.1 | VM batch guest memory contract | `d7b6ebc` |
| P38.2 | Runtime 原子 Transform Batch 与真实 WAMR benchmark | `18bc18f` |
| P38.3 | 统一 Typed Gameplay Event Contract | `b08aa47` |
| P38.4 | UE Collision Delegate 接入 | `082e04f` |
| P38.5 | C# 生成式 Collision Callbacks | `09e8aa6` |
| P38.6 | Typed Input Ingress 与 C# InputEvent | `2893432` |
| P38.7 | 全模块、聚焦、完整自动化与中文文档收尾 | 本次收尾 |

## 现在可以做什么

### C# gameplay lifecycle

C# 脚本已经可以实现：

- `BeginPlay()`
- `Tick(float deltaSeconds)`
- `EndPlay()`
- `OnTimer(int callbackId, int timerHandle)`
- `OnEvent(int eventId, float value)`
- `OnBeginOverlap(AActor otherActor, FVector location)`
- `OnEndOverlap(AActor otherActor, FVector location)`
- `OnHit(AActor otherActor, FVector normalImpulse)`
- `OnInput(InputEvent input)`

这些 callback 能在真实 WAMR 工件中调用现有 typed Actor APIs，修改 owner 或 OtherActor 的位置、旋转、scale 和 RootComponent world location。

### UE 事件接入

`UAvidScriptComponent` 已经：

- 在 Running 后绑定 owner BeginOverlap、EndOverlap 和 Hit delegates。
- 把 OtherActor 注册成 session-owned generation handle。
- 在 Stop、Fault 和 Unload 前解绑 delegates。
- 通过 `DispatchScriptInput(ActionId, TriggerEvent, Value)` 接受 Blueprint 或任意项目输入系统。
- 对 invalid payload fail closed，但不错误卸载健康 runtime。
- 对 guest trap 保留 root cause 并立即停止脚本实例。

Runtime 不依赖 EnhancedInput。项目可以在 Enhanced Input callback、传统 Input、AI、网络回放或测试代码中调用统一 input ingress。

### Typed UE API

当前 C# 子集已经具备：

- handle-backed `AActor` / `USceneComponent`
- `UE.Self`
- `FVector` / `FRotator` / `FTransform` snapshot
- Actor location、rotation、scale typed get/set
- Actor world offset
- RootComponent 获取与 world location read/write
- runtime-owned one-shot Timer
- static float 状态与基础表达式

### Editor workflow

当前可以从 Editor profile 完成：

```text
C# source
  -> source adapter
  -> WASM + report + manifest
  -> bind selected Actor
  -> PIE BeginPlay/Tick/Timer/Event/Collision/Input/EndPlay
```

自定义脚本缺少可选 callback 时，生成器仍提供稳定 no-op 导出；不会因 callback 未声明而破坏旧模块。

## 统一事件 ABI

通用导出固定为：

```text
avid_on_gameplay_event(
    eventType,
    primaryId,
    secondaryId,
    objectSlot,
    objectGeneration,
    vectorX,
    vectorY,
    vectorZ)
```

BeginOverlap、EndOverlap、Hit 和 Input 只是 descriptor table / Schema 中的条目。新增事件不应创建新的 export，也不应把 UE 类型泄漏到 VM public contract。

## Batch 性能结果

Phase 38 最终 benchmark：

```text
transform_batch = 64 Actors
scalar WASM imports = 192
batch WASM imports = 1
wasm scalar transform avg = 0.135110 ms
wasm batch transform avg = 0.029634 ms
speedup = 4.56x
```

同一轮其他数据：

- binding get avg: 0.000249 ms
- binding set avg: 0.000435 ms
- runtime Tick avg: 0.0006 ms
- runtime Unload avg: 0.0065 ms
- 512 pending timers idle Tick avg: 0.000394 ms

这些数据是开发机 Editor/NullRHI microbenchmark，不等价于 Shipping、移动端或最终游戏负载，但证明减少 WASM crossing 的方向有效。

## 验证矩阵

| 门禁 | 结果 |
| --- | --- |
| PowerShell parser | 成功 |
| Architecture gate | 成功 |
| Core build | 成功，up to date |
| VM build | 成功，up to date |
| Bindings build | 成功，1 link action |
| Runtime build | 成功，3 actions |
| Editor build | 成功，3 actions |
| VM 聚焦 | 5/5 |
| Runtime Transform Batch | 1/1 |
| Reload | 8/8 |
| Performance | 3/3 |
| C# | 4/4 |
| Component / Collision / Input | 6/6 |
| 完整 AvidScript | 137/137，Fail 0 |

日志：

- `Saved/Logs/AvidScript_P38_VM.log`
- `Saved/Logs/AvidScript_P38_Batch.log`
- `Saved/Logs/AvidScript_P38_Reload.log`
- `Saved/Logs/AvidScript_P38_Performance.log`
- `Saved/Logs/AvidScript_P38_6_CSharp_Final.log`
- `Saved/Logs/AvidScript_P38_6_Component.log`
- `Saved/Logs/AvidScript_P38_All.log`

完整日志机器计数结果：

```text
Found: 137
Success: 137
Fail: 0
Performed: 137
```

## 尚未达到成熟方案的部分

Phase 38 完成的是 gameplay 闭环和扩展架构，不代表已经达到 Puerts、UnLua 或成熟 AngelScript integration 的整体成熟度。当前仍缺：

- 正式 lexer/parser/AST/type checker/IR，而不是 source adapter 子集。
- 从 UE Reflection Schema 自动生成 C# facade、host dispatcher、manifest 和诊断。
- 覆盖 UObject/UClass/UFunction/UPROPERTY、容器、delegate、Spawn、组件与资产系统的生成式绑定。
- 断点、调用栈、source map、变量查看和调试协议。
- 多脚本实例规模化调度、GC/handle 压力和长时间 soak。
- PC packaged Development/Shipping 的本阶段回归。
- Android/iOS WAMR/AOT、工具链、ABI 和性能验证。
- 稳定版本策略、兼容矩阵、升级工具和发布流程。

## 后续主线

- Phase 39：正式语言前端骨架，建立 source span、token、AST 和确定性 diagnostics。
- Phase 40：类型系统与语义分析，覆盖生命周期、值类型、对象句柄和 callback signatures。
- Phase 41：IR 与 WASM codegen，逐步替换 PowerShell source adapter。
- Phase 42：UE Reflection Binding Generator，生成 facade/dispatcher/manifest，不再逐 API 手写。
- Phase 43 以后：容器、delegate、异步、调试、热重载状态迁移、PC packaging、移动端与发布工程。

成熟度目标不变：沿着统一 ABI、正式前端和反射生成路线继续前进，禁止用示例数量或手写 API 数量伪装平台成熟度。
