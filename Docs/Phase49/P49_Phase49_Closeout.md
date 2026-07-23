# Phase 49 收尾报告

## 阶段结论

Phase 49 已完成项目级 UE Binding Profile、缓存类引用、可信 World 注入、C# 对象生命周期 API、动态投射物游戏逻辑闭环，以及 Native/Binding 对照性能基线。

当前框架已能让 C# Guest 在 `BeginPlay`、`Tick` 和 Timer 等 UE 生命周期中，使用生成 API 创建指定 Actor/Blueprint 类、保存安全对象句柄、调用普通授权 UFunction、执行 `IsA` 判断并销毁对象。新增能力继续走 descriptor、authorization package、Guest IR、WASM backend、WAMR host import 与 Runtime Session 的统一生成链路，没有为每个 UFunction 手写 WAMR wrapper。

## 本阶段交付

### 项目级 Binding Profile

- profile schema 支持 module path、显式 class include/exclude、class rule 和 class reference；
- discovery、规范化、稳定排序和 selection hash 保证相同声明得到相同内容寻址结果；
- UFunction 与 property 资格复用共享 reflection selection 策略；
- build request 将 profile 解析与 BuildPipeline 解耦，同步构建、异步热重载和旧执行器复用同一合同；
- binding package 按实际所需 UE function 与授权 stable ID 发现，不再依赖“最新文件”猜测能力。

### 缓存类引用

- descriptor schema v5 发布 class table、ordinal、base constraint 和 load policy；
- package load 时完成 UClass 路径解析、继承校验、不可创建类拒绝和强引用锚定；
- Runtime 热路径通过 immutable ordinal plan O(1) 取类，不执行字符串 class load；
- C# 侧生成 nominal `class_ref`，普通整数与 class reference 之间失败关闭。

### World 与对象生命周期

- Runtime Session 由受信宿主注入 World，Guest 不能伪造 World 指针；
- `UE.SpawnActor` 从缓存 class ordinal 与 Guest `FTransform` 创建 Actor；
- `UE.DestroyActor` 释放 generational handle，旧 slot/generation 再解析会被拒绝；
- `UE.IsA` 复用缓存类计划进行只读类型判断；
- Spawn/Destroy 在热重载候选阶段失败关闭，避免未提交候选产生不可逆 World 副作用。

### 可运行游戏逻辑

样例位于：

- `Samples/CSharp/DynamicProjectile/DynamicProjectileScript.cs`；
- `Samples/CSharp/DynamicProjectile/DynamicProjectile.csharp-profile.json`；
- `Samples/CSharp/DynamicProjectile/README.md`。

样例在 `BeginPlay` 安排 Timer，随后创建目标 Blueprint 投射物；`Tick` 读取位置并修改缩放；后续 Timer 销毁投射物。端到端测试同时验证动态类型、初始 Transform、连续 Tick、registry live handle 数量、热重载回滚和销毁后的 generation mismatch。

## 最终 Gate

冻结候选：`99978a8f5390b0891ee5066b9dcd5476f64b3d47`。

| 门禁 | 结果 |
| --- | ---: |
| .NET 自有测试 | 150/150 |
| .NET format | 5/5 |
| PowerShell 合同 | 105/105 |
| PowerShell parser | 22/22 |
| 架构门禁 | 通过 |
| UE5.8 四模块 no-clean build | 通过 |
| 完整 `Automation RunTests AvidScript` | 232/232 |
| 完整 Automation 非成功结果 | 0 |
| Queue Empty / TestExit / RequestExit status | 全部通过，status 0 |
| 聚焦 typed performance 复测 | 1/1 |

完整 Automation 只执行一次。由于完整套件中的 registry 微基准为 `0.000188 ms`，比 Phase 48 高约 6.2%，按既定阈值追加一次聚焦性能复测；复测为 `0.000174 ms`，相对 Phase 48 的 `0.000177 ms` 约降低 1.7%，5% 回退门禁通过。

## 性能结果

### Typed Binding 回归

| 指标 | Phase 48 | Phase 49 聚焦复测 | 变化 |
| --- | ---: | ---: | ---: |
| Registry resolve | 0.000177 ms | 0.000174 ms | -1.69% |
| Binding GetActorLocation | 0.000258 ms | 0.000047 ms | -81.78% |
| Binding SetActorLocation | 0.000460 ms | 0.000206 ms | -55.22% |
| Scalar GetActorTransform | 0.000285 ms | 0.000067 ms | -76.49% |
| Batch GetActorTransforms | 0.000289 ms | 0.000071 ms | -75.43% |
| WASM scalar transform | 0.152559 ms | 0.075870 ms | -50.27% |
| WASM batch transform | 0.030585 ms | 0.013335 ms | -56.40% |

### 对象生命周期基线

| 路径 | P50 | P95 |
| --- | ---: | ---: |
| Native SpawnActor | 0.006750 ms | 0.008594 ms |
| Binding SpawnActor | 0.007105 ms | 0.008003 ms |
| Native DestroyActor | 0.008584 ms | 0.012065 ms |
| Binding DestroyActor | 0.008487 ms | 0.011728 ms |
| Class ordinal resolve | 0.000004 ms | 未单列 |
| Registry resolve | 0.000013 ms | 未单列 |

Binding package load 观察到 1 次 class load、0 次 reflected member lookup；warm/timed loop 中 class load 与 reflected lookup 均为 0。真实 WAMR probe 观察到 2 次 host import，结构上分别对应一次 Spawn 和一次 Destroy。

这些数据是当前机器上的 UE5.8 Development Editor 基线，不构成 Shipping、移动端或外部脚本框架的绝对性能排名。

## 架构状态

- Core 只拥有通用合同；
- Bindings 拥有 UE typed API、descriptor 与缓存 dispatch plan，不依赖 WAMR；
- VM 拥有 WAMR、Guest Memory 和 host import crossing；
- Runtime 负责 World、Session、对象 registry、生命周期与模块组合；
- Editor 负责 profile 解析、reflection 发现、生成、构建与热重载编排。

成功热路径显式使用轻量诊断策略，避免无用 `GetPathName()`；公共 Binding API 仍默认保留完整 `ObjectPath` 诊断，失败路径保持可操作错误信息。

## 当前边界

- 已支持 profile 授权范围内的一般 UFunction/property 与 Actor 生命周期能力，但还不是任意 UObject factory；
- 尚未实现动态组件创建、Outer/ownership policy、Spawn/Destroy 候选副作用补偿；
- 尚未完成 Shipping/Cook、Android/iOS AOT 和移动端性能验收；
- C# 前端仍需扩展更多 Roslyn CFG/location lowering、泛型与语言特性覆盖；
- 尚未建立与 Puerts、UnLua、AngelScript 的同机同项目 benchmark，因此不宣称外部排名领先。

## 流程复盘

- 最终 .NET 测试最初错误并发，触发共享 `obj/Release` 文件锁；已改为共享 project graph 永久串行；
- callable 数量夹具与新增 `AActor.Matches` 漂移；已同时断言总数与新增 method symbol；
- prepared semantic 夹具曾按最新时间选择不兼容 package；已改为按 required UE function 与授权 stable ID 选择；
- 首次 Gate 报告生成假设成功且无输出的 `dotnet format` 仍会创建日志，导致证据生成被拒绝。修正后 wrapper 无论工具是否输出，都显式写入成功标记；失败证据目录保持不变，正式报告写入新的不可变目录。

## 后续方向

下一阶段应围绕通用对象构造与 ownership policy、可补偿热重载副作用、更多生成式 UE 类型投影、Cook/Shipping 闭环和可重复 benchmark harness 展开。继续坚持“由 reflection/profile 生成能力、Runtime 使用缓存 plan、VM 只负责 crossing”的路线，避免退化为逐 API 手写绑定。
