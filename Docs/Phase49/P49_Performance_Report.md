# Phase 49 对象生命周期性能报告

## 结论

Phase 49 已建立首份同机、同 World、同 Actor class 的 Native 与 AvidScript Binding 对象生命周期基线。当前结果显示，在本次 UE5.8 Development Editor 测量中，Binding Spawn/Destroy 的 P50 与 Native 处于同一数量级；这是一份后续回归使用的项目内基线，不是跨机器绝对阈值，也不用于宣称优于 Puerts、UnLua 或 AngelScript。

对象生命周期性能自动化为：

```text
AvidScript.Performance.ObjectLifecycleBenchmarkSmoke
```

## 测量方法

测试配置：

| 参数 | 值 |
| --- | ---: |
| Warmup | 3 轮 |
| Samples | 20 轮 |
| Iterations per sample | 128 |
| Target | UE5.8 Win64 Development Editor |
| Rendering | NullRHI |

每轮分别测量：

- Native `UWorld::SpawnActor`；
- AvidScript `FAvidScriptBindingPackage::Dispatch` Spawn；
- Native `AActor::Destroy`；
- AvidScript `FAvidScriptBindingPackage::Dispatch` Destroy；
- immutable class ordinal resolve；
- generational object registry resolve。

Native 与 Binding 的执行顺序逐轮交替，减少固定先后顺序带来的缓存、World 增长和系统调度偏差。descriptor 构建、class load、ordinal 查找、World 创建、数组扩容和 Destroy fixture 创建均不进入相应计时区间。每轮结束要求 registry 回到唯一 resolve fixture handle，最终要求 live handle 为 0。

统计统一使用 nearest-rank P50/P95。固定数据集 `1..20` 的合同结果为 P50=`10`、P95=`19`，不再把 20 个样本的 P95 错算成最大值。极小耗时保留原始测量，不再强制抬高到 `0.0001 ms`。

## 本机基线

日志：`<Temp>/AvidScript_P49_4_LifecycleBenchmark_FinalFocused.log`

| 路径 | P50 | P95 | Binding 相对 Native |
| --- | ---: | ---: | ---: |
| Native SpawnActor | 0.006396 ms | 0.007540 ms | 基准 |
| Binding SpawnActor | 0.006536 ms | 0.008213 ms | P50 +2.19%；P95 +8.93% |
| Native DestroyActor | 0.008038 ms | 0.011365 ms | 基准 |
| Binding DestroyActor | 0.007780 ms | 0.011507 ms | P50 -3.21%；P95 +1.25% |
| Class ordinal resolve | 0.000003 ms | 本轮摘要未单列 | 不适用 |
| Registry resolve | 0.000012 ms | 本轮摘要未单列 | 不适用 |

负差值只表示本轮采样噪声下 Binding 数值略低，不代表 Binding 比 Native 更快。对象创建和销毁的主要成本仍来自 UE World/UObject 生命周期；AvidScript 的缓存计划、句柄注册与安全检查在这组样本中没有改变数量级。

## Crossing 证据

计时主循环刻意从 C++ 直接进入 binding dispatch，用于隔离 host binding 成本；它本身不能证明 guest-to-host crossing 数。

因此测试另建了真实 WASM 模块，通过 WAMR 在同一个 `BeginPlay` 中：

1. 调用一次 `avidscript.avid_object_spawn_actor`；
2. 从 Guest Memory 读取返回的 slot/generation；
3. 调用一次 `avidscript.avid_object_destroy_actor`。

Runtime 实际观察到 `HostImportCallCount=2`，调用后 registry live handle 回到 0。probe 模块的静态结构固定为一次 Spawn import 后紧接一次 Destroy import，因此可以确认这条成功路径共发生 2 次 crossing、每个操作各 1 次。当前 Runtime telemetry 只记录总调用数，没有为这两个 import 分别维护长期计数器；该限制不会影响结构验证，也避免为生产热路径增加专用 benchmark 计数。该 probe 用于结构正确性，不混入 Native/Binding P50/P95。

## Load 与 Lookup 证据

`FAvidScriptBindingPackage` 现在公开只读 instrumentation：

- `ClassLoadCount` 在 class path cache miss 并进入 `LoadObject<UClass>` 前递增；
- `ReflectedNameLookupCount` 在 property/function reflection lookup 前递增；
- dispatch 热路径不修改这两个计数。

本次 class-only package 的观测结果：

| 阶段 | Binding Package class load | Binding Package reflected member lookup |
| --- | ---: | ---: |
| Package load | 1 | 0 |
| 全部 warm/timed loop | 0 | 0 |

class path 与 base path 都是 `/Script/Engine.Actor`，由 package class cache 合并为一次 cache miss 后的 `LoadObject` 调用。该 instrumentation 不判断 UClass 此前是否已驻留，也不把调用次数解释为磁盘加载次数。普通项目 UFunction 的 reflected lookup 仍发生在 package load，后续 dispatch 使用缓存 plan。

## 成功热路径诊断成本

对象 registry 保留原有公共默认：Register、Resolve、Release 成功时可返回 `ObjectPath`。typed Actor/SceneComponent Binding 也保留公开默认的完整诊断，并允许 Runtime 通过 `EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath` 显式选择轻量模式。生产高频路径现在统一选择轻量策略：

- reflected UObject return 注册；
- Actor/SceneComponent resolve；
- lifecycle Spawn 注册；
- lifecycle Destroy resolve 与 release；
- gameplay event 临时对象句柄。

失败路径仍构造对象路径和可操作诊断；组件 owner 的冷启动注册继续显式保留路径。这样没有静默改变默认 API 语义，同时消除成功热路径无用的 `GetPathName()`。

## Typed Binding 回归

Phase 48 同机历史基线：

| 指标 | Phase 48 Avg |
| --- | ---: |
| Registry resolve | 0.000177 ms |
| Binding GetActorLocation | 0.000258 ms |
| Binding SetActorLocation | 0.000460 ms |
| Scalar GetActorTransform | 0.000285 ms |
| Batch GetActorTransforms | 0.000289 ms |

Phase 49 冻结后的完整 performance filter 会重新运行同一 `HostBindingOverheadSmoke`。最终 Gate 以同机新旧 Avg 对比，要求已有 typed binding 指标没有超过 5% 的回退；若采样噪声触发阈值，将增加重复采样并报告分布，而不是修改阈值或隐藏结果。

## 解释边界

- 当前 benchmark 是 Editor/PC 基线，不代表 Shipping、Cook、Android 或 iOS；
- 没有测量 C# authoring 编译耗时、首次 package build 或 Blueprint class 首次加载；
- WAMR crossing probe验证次数，不用于推导 Spawn/Destroy 的 WASM 端到端延迟；
- 未在同机同项目下运行外部框架，因此不做 Puerts、UnLua、AngelScript 的绝对性能排名；
- Phase 49 数据是首次对象生命周期 frozen baseline，后续阶段应保持同一方法并按提交记录趋势。
