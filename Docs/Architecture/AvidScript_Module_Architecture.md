# AvidScript 模块架构

日期：2026-07-12
状态：Phase 36 Runtime Session 架构已落地

## 1. 架构目标

AvidScript 不再把 VM、UE Binding、脚本生命周期和编辑器工具集中在单个模块。项目采用单向依赖、唯一资源所有者和可替换后端，保证 PC 先行实现不会阻塞后续移动端、AOT 与其他 VM 后端。

```text
AvidScriptCore
    ^          ^
    |          |
AvidScriptVM  AvidScriptBindings
    ^          ^
     \        /
    AvidScriptRuntime
           ^
           |
    AvidScriptEditor
```

依赖规则：

- `AvidScriptCore` 只依赖 UE `Core`，保存生命周期、诊断与跨模块基础契约。
- `AvidScriptVM` 依赖 Core，WAMR 只允许出现在 VM 私有实现中；VM 不认识 `UObject`、`AActor`、`FVector` 等 UE gameplay 类型。
- `AvidScriptBindings` 依赖 Core、CoreUObject 和 Engine，实现对象句柄及类型化 UE API；不得包含 WAMR API。
- `AvidScriptRuntime` 组合 VM 与 Bindings，拥有脚本 session、reload transaction、scheduler 和 UE integration。
- `AvidScriptEditor` 只负责构建、profile、Schema 生成和编辑器交互，不能向运行时泄漏 Editor 类型。

`Build/CheckAvidScriptArchitecture.ps1` 是当前可执行的模块墙。后续增加模块或移动代码时，必须同步维护此脚本。

## 2. 生命周期与资源所有权

生命周期状态由 `FAvidScriptLifecycleStateMachine` 批准：

```text
Empty -> Loaded -> Starting -> Running -> Stopping -> Stopped
                    |             |
                    +-> Faulted <-+
```

- 同状态转换幂等成功，但不会重复执行 guest callback。
- 非法转换必须失败，不能通过额外 bool 绕过状态机。
- guest trap 进入 `Faulted`，停止继续 Tick、Timer 和 Event。
- `Unload` 清理资源并回到 `Empty`。
- 析构只做幂等兜底，正常路径必须显式 Stop 与 Unload。

目标资源 owner：

| 资源 | 唯一 owner | 生命周期 |
| --- | --- | --- |
| WAMR 全局 lease | VM backend service | 首个实例到最后实例 |
| module / instance / exec env | VM instance | Load/Instantiate 到 Unload |
| export cache | VM instance | ABI resolve 到 Unload |
| UObject registry | UE Integration（Component） | Component BeginPlay 到 EndPlay |
| Timer storage | Runtime instance | Load 到 Unload |
| Tick/Event route | Session Scheduler/EventRouter | active attach 到 detach |
| UE delegates | Integration adapter | Running 到 Stop/Fault |
| reload candidate | Reload transaction | prepare 到 commit/rollback |

Phase 36 已由 `FAvidScriptRuntimeSession` 统一 active runtime、manifest、host context、reload transaction 与路由状态。Component/WorldSubsystem 不再持有 direct Runtime，也不再保存兼容 active bool；观测 stats 不参与状态决策。

## 3. Binding 设计

对象跨 WASM 边界使用 `slot + generation`，不暴露原生指针：

- `slot` 定位句柄槽位。
- `generation` 防止释放并复用后的陈旧句柄访问。
- UObject 到 slot 的反向索引使重复注册均摊 O(1)。
- 弱引用失效、Release 和 Reset 必须同时清除反向索引。
- 类型化 provider 负责 `AActor`、`USceneComponent`、`FVector`、`FRotator` 等 UE 语义。

后续 host call 通过稳定 `BindingId + POD call frame` 路由。VM 只传递无 UE 类型的调用帧，Bindings 完成类型校验和执行。

## 4. Reload 与事件

Reload 使用 candidate transaction：

```text
Load candidate -> Validate ABI -> Start candidate -> Atomic swap -> Unload old
```

candidate 在 Load、Validate 或 Start 阶段失败时，active session 保持不变。热重载不是 UE EndPlay：成功替换直接卸载旧实例，`avid_on_end_play` 只由真实 Component/World EndPlay 或显式 Stop 触发；后续 reload cleanup/state migration 使用独立协议。UE 的 BeginPlay、Tick、EndPlay、Timer、Input 和 Delegate 只在 Runtime Integration 层转换为稳定调用，VM backend 不直接订阅 UE delegate。

## 5. 性能原则

- Load/Validate 时解析并缓存生命周期和事件 exports，Tick 热路径不得反复按名称查找。
- 成功调用返回轻量 POD；仅失败路径构造详细字符串诊断。
- 已知 UObject 重复注册使用反向索引，不进行全槽扫描。
- 高频 struct 使用 guest memory view 或 scratch arena，避免临时堆分配。
- 跨 WASM 调用按收益提供批量 API，优先减少边界穿越次数。
- Timer 后续使用 deadline min-heap 或 timing wheel，避免每 Tick 全数组扫描。
- Shipping 默认关闭逐调用计时，按 session 采样统计。
- WAMR interpreter、Fast JIT、AOT 与移动端配置属于 VM backend policy，不渗入 Binding。

性能变更必须同时提供正确性测试和 benchmark。当前门槛是 typed binding microbenchmark 相对既有基线不得回退超过 5%。

## 6. 迁移状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| Phase 34 | Core、Bindings、状态机、反向索引、模块墙 | 已完成 |
| Phase 35 | VM backend、WAMR 私有化、export cache | 已完成 |
| Phase 36 | Runtime Session、Reload、Scheduler/EventRouter 统一所有权 | 已完成 |
| Phase 37 | 热路径结果、Timer、批量 Binding、性能报告 | 已完成 |
| Phase 38 | 批量 WASM ABI、typed overlap、Hit、Input、Delegate | 进行中：P38.1-P38.2 已完成 |

Phase 37 已完成成功路径结果、Timer 数据结构和 typed Transform Batch。Phase 38 正在把 Batch 接入真实 WASM ABI，并通过统一 Gameplay Event Contract 接入 Overlap、Hit 和 Input；新的 Gameplay 入口继续进入 Integration/EventRouter，不能回流 VM backend 或绕过 Session。Phase 39 起主线转入正规语言前端，Phase 42 转入 Reflection Binding Generator，禁止以逐个手写 UE API 代替生成式覆盖。
