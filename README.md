<div align="center">

# AvidScript

**面向 Unreal Engine 的现代 C# + WebAssembly 游戏脚本框架**

<p>
  <img alt="Unreal Engine 5.8" src="https://img.shields.io/badge/Unreal%20Engine-5.8-0E1128?logo=unrealengine&logoColor=white">
  <img alt="C# Guest" src="https://img.shields.io/badge/Guest-C%23-512BD4?logo=dotnet&logoColor=white">
  <img alt="WebAssembly" src="https://img.shields.io/badge/Target-WebAssembly-654FF0?logo=webassembly&logoColor=white">
  <img alt="Wasmtime 45" src="https://img.shields.io/badge/VM-Wasmtime%2045-2B6CB0">
  <img alt="Win64 Validated" src="https://img.shields.io/badge/Platform-Win64%20Validated-0078D4?logo=windows&logoColor=white">
  <img alt="Android arm64 Cross-AOT" src="https://img.shields.io/badge/Android%20arm64-Cross--AOT-3DDC84?logo=android&logoColor=white">
  <img alt="Phase 64 Active" src="https://img.shields.io/badge/Status-Phase%2064%20Active-2B6CB0">
  <img alt="Automation Baseline 439/439" src="https://img.shields.io/badge/Baseline-439%2F439-26A269">
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/License-MIT-2E8B57"></a>
</p>

AvidScript 将 C# 编译为 WASM，通过自动生成的 Binding 接入 UE 生命周期、项目 API、网络与异步流程。
Win64 主后端使用 Wasmtime 45，保留 WAMR 兼容后端；UE Runtime 不托管 CLR。

</div>

> [!IMPORTANT]
> 当前版本为 **0.1.0 开发者预览**，主线验证环境是 **UE5.8 源码版 + Win64
> Development/Shipping + Wasmtime 45**。两种 Win64 配置的 BuildCookRun 均已通过；Android arm64
> 交叉 AOT 发布已验证，Android UBT/真机与 iOS 仍待正式验收。

## 现在可以做什么

更新于 **2026-09-04**，**P64 实施中**。已能用 C# 编写 UE 游戏逻辑、调用项目自定义 API，
并运行 Win64 打包样例；不需要 `.avid`，也不是完整 UE/.NET 替代层。

最近补齐 **UI/存档异常处理与组件退出验证**：失败读取保留原对象、Reset、保存失败、取消订阅与迟到事件隔离。
普通 UObject 授权、独立事件来源查询和无需 Tick 的脚本也已交付。当前能力如下：

| 领域 | 已实现内容 |
| --- | --- |
| 游戏流程 | `BeginPlay/Tick/EndPlay`、Timer、Overlap、Gameplay Event；事件型 Guest 可省略 Tick，Startup Scenario 自动挂载与回滚 |
| C# 玩法 | [PickupRush](Samples/CSharp/PickupRush/README.md)：计时、收集、复活和胜负；Editor、Win64 Development/Shipping 包内闭环已验证 |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md)：UMG 按钮、计分、SaveGame；正常存取、缺档/GC、错误类型/分数、Reset 与组件退出，五进程验证通过 |
| 生成式 UE API | Reflection/Profile 生成 `UFUNCTION`、`UPROPERTY`、Interface 与项目自定义 API；typed Self、Spawn、cast、销毁，无需逐个手写 VM wrapper；普通 UObject 的 owned/borrowed 授权与 Self 缓存隔离 |
| 类型与容器 | UObject、向量/变换、固定 `USTRUCT`、名称/字符串、`FText`；字符串数组、递归容器、`TSet/TMap`；soft/weak object 身份往返，详见下方边界 |
| C# 定义 UE 类型 | Actor、Component、World/GameInstance Subsystem，含继承、override、属性、函数与默认参数 |
| 异步与委托 | 受控 `async/await`、Delay/NextTick、异步加载、Latent、AsyncAction；单播/多播、受支持签名的 `return/ref/out` 与主动调用；独立 UObject 订阅与回调来源查询 |
| Blueprint 与网络 | callable/event 双向交互；Server/Client/NetMulticast RPC、属性复制与 RepNotify，dedicated/listen 多进程验证 |
| 热重载与安全 | 方法体事务式替换与回滚；ObjectHandle、Session 隔离、执行预算、后台恢复、低内存回收及 teardown 自动取消 |
| Cook 与发布 | 内容寻址模块、多平台 catalog、无头 Release、Generated Type 预编译；Win64 Development/Shipping BuildCookRun 与回执校验 |
| VM 后端 | Wasmtime 45 Win64 JIT/AOT；Android arm64 脚本及 Generated Type 交叉 AOT；WAMR 兼容后端 |
| 构建与 IDE | 增量缓存、persistent Worker、`.slnx`/WASI 工作区、离线源码索引与 Visual Studio/Rider/VS Code 启动 |
| 调试与 Profiler | 源码映射、跨层调用栈、PIE 目标、受控同步断点/步进、只读变量；UE Trace、热点与 JSON 导出 |

类型覆盖见 [P58 验收](Docs/Phase58/P58.4_Centralized_Gate_Report.md)，最新交付见
[P64 记录](Docs/Phase64/P64_Closeout.md)。真实输入/视觉、包内 UI、World 销毁、热重载压力、长稳和移动端仍待验收。

## C# 游戏脚本

直接阅读随样例构建的源码，而不是另写一套展示代码：

- [PickupRushScript.cs](Samples/CSharp/PickupRush/PickupRushScript.cs)：完整计时收集玩法。
- [UiSaveDemoScript.cs](Samples/CSharp/UiSaveDemo/UiSaveDemoScript.cs)：按钮订阅、文本更新、保存/加载和清理。
- [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)：生命周期、Timer 与受控异步。

项目类型与方法来自 Reflection/Profile 生成的 C# API；运行方式见下方样例链接。

## 架构

```mermaid
flowchart LR
    Reflection["UE Reflection<br/>Binding Profile"] --> Generator["AvidScriptEditor<br/>Generator"]
    Generator --> API["Generated C# API"]
    Generator --> Package["Immutable Binding Package"]
    Script["C# Script"] --> Semantic["Roslyn Semantic<br/>Guest IR"]
    API --> Semantic
    Semantic --> Wasm["WebAssembly"]
    Wasm --> VM["Wasmtime / WAMR"]
    Package --> Runtime["Session + Prepared Plans"]
    VM --> Runtime
    Runtime --> UE["Unreal Engine 5.8"]
```

| 模块 | 职责 |
| --- | --- |
| `AvidScriptCore` | 后端无关 ABI、错误与基础合同 |
| `AvidScriptBindings` | descriptor、codec、prepared executor 与 value heap |
| `AvidScriptVM` | Wasmtime/WAMR、WASM 校验、Guest Memory 与 Host crossing |
| `AvidScriptRuntime` | Session、生命周期、对象 registry、事件、异步与热重载 |
| `AvidScriptEditor` | Reflection/profile、代码生成、C# 构建和 Editor 集成 |
| `Tools/` | Roslyn 前端、Guest IR 与 WASM backend |

对象始终通过 generational `ObjectHandle` 访问；不向 Guest 暴露原始 `UObject*`。不支持的类型或
失配的反射身份会在生成或加载阶段失败关闭。

## 性能摘要

以下为已归档基准，**没有对当前 P64 改动重新测量**。主表来自 P57：UE5.8 Win64 Development、
Intel Core Ultra 7 265K、Wasmtime 45 Cranelift JIT，与冻结版本的 Puerts V8 同机对照；
5 个计时进程，每进程 5 次预热、每单元 30 次采样。比率为 AvidScript / Puerts，越低越好。

![Prepared Reflection 性能对比](Docs/Assets/README/phase57-prepared-reflection-performance.svg)

| UE 交互场景 | AvidScript P50 | Puerts Reflection P50 | 比率 |
| --- | ---: | ---: | ---: |
| Scalar UFUNCTION | `54.57 ns` | `106.15 ns` | **`0.514x`** |
| Property get/set | `68.14 ns` | `103.01 ns` | **`0.661x`** |
| FVector value | `66.49 ns` | `1193.10 ns` | **`0.056x`** |
| UObject roundtrip | `69.60 ns` | `130.90 ns` | **`0.532x`** |

以上是指定 prepared/fused 路径的每逻辑操作耗时，不代表任意 `UFUNCTION` 都有相同收益。
环境、路由计数与正确性结果见 [P57 原始证据](Docs/Phase57/P57.11B1_Recursive_Fixed_Struct_Codec_Evidence.json)。

Phase 56 完整游戏 workload 中，Small gameplay、Dense gameplay 与 Lifecycle callback 的
P50 比率分别为 **`0.469x`、`0.513x`、`0.391x`**，详见
[游戏 workload 报告](Docs/Phase56/P56.5_Fused_Call_Frame_Implementation_Report.md)。

**尚未全面领先：** 12-kernel 相同 WASM 的 Wasmtime/V8 对照中，P50/P95 几何均值为
`0.9800x / 1.0006x`，均未达到冻结的 `<= 0.95x` 目标；这不是 C# 与 JavaScript 的完整游戏比较。
纯执行领导力门禁仍未关闭，也没有同口径 UnLua/AngelScript 排行榜。见
[执行层报告](Docs/Phase57/P57.13_Cranelift_Speed_Profile.md)。

其他归档结果：[容器基准](Docs/Phase57/P57.11D_Compiler_Managed_Array_Region.md)、
[UE 原生对照](Docs/Phase60/P60.D_Performance_And_Gate.md)、[增量构建](Docs/Phase61/P61.E_Integration_Gate.md)。

## 快速开始

要求：UE5.8 源码版、Windows 10/11 x64、Visual Studio 2022、.NET SDK `8.0.416`、
PowerShell 7 与 Git。

1. 将仓库放入项目的 `Plugins/AvidScript`。
2. 在插件目录安装锁定的 Wasmtime 依赖：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

3. 使用源码版 UE5.8 增量构建 Editor Target：

```powershell
$env:UE_ROOT = "C:\UnrealEngine"

& "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development `
  "-Project=C:\Path\To\YourProject.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

4. 构建仓库内第一个 C# 生命周期 Guest：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

项目自定义 API 需要先通过 Editor Reflection 与 Binding Profile 生成 binding package 和 C# facade。

## 样例

| 样例 | 展示内容 |
| --- | --- |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | typed Self、自定义 UFUNCTION、Spawn、cast 与销毁 |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 生命周期、Timer、异步加载与受控 async/await |
| [PickupRush](Samples/CSharp/PickupRush/README.md) | Startup Scenario、真实地图自动挂载、计时收集与胜负闭环 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UMG 按钮、计分与 SaveGame 落盘/重启读回；Editor 游戏进程验证 |
| [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) | Overlap、Gameplay Event、持久状态与热重载 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | C# 调用项目 Server、Client 与 NetMulticast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | replicated property、authority setter 与 Push Model |
| [NetworkTopology](Samples/CSharp/NetworkTopology/README.md) | dedicated/listen 多进程网络闭环 |

## 当前边界

- **UE 类型**：由 Profile 与 ABI/codec 决定生成范围，并非所有 UE API 自动可用。复合容器内强 UObject 引用仍拒绝，平面 `TArray<UObject*>` 可用；Set/Map key 受确定性编码限制，soft/weak 的脚本侧解析易用接口待补齐。
- **C# 子集**：无完整 .NET Runtime、任意 awaiter 或异常系统；暂不支持 `event +=`、lambda/closure，使用显式 bind/subscribe 与 `ExecuteX/BroadcastX`。
- **重载与隔离**：方法体可热重载；反射结构变更需增量 UBT 并重启 Editor。WASM 故障隔离不等于原生 C++/DLL 进程沙箱。
- **玩法与平台**：UI/存档通过 Editor 合成事件验证，不等于真实输入/视觉、任意损坏存档、World 销毁、包内 UI 或热重载长稳验收；Android UBT/APK/真机及 iOS 尚未验收。
- **诊断与性能**：typed Host 的具体拒绝原因尚未统一透传到 VM 错误；纯执行 P50/P95 领先门禁未关闭，也未完成同口径 UnLua/AngelScript 矩阵。

下一步补齐 P64 的 UI 真实输入/包内运行、World/热重载压力与长稳验证，以及移动构建/设备证据；
随后推进安装、升级、兼容和诊断等发布工程，不以阶段编号代替实际验收。

## 验证

完整回归与后续专项分别记录，**不累加成当前全量通过数**：

| 范围 | 已归档证据 |
| --- | --- |
| [完整技术基线](Docs/Phase64/P64_Closeout.md)，候选 `9e08cdc` | Automation **439/439**、.NET **284/284**；10 组 PowerShell 合同、干净候选架构检查和 no-clean Editor UBT 通过 |
| [PickupRush](Samples/CSharp/PickupRush/README.md) | Editor / Win64 Development / Shipping 均为 **5/5** 事件与胜利状态；包回执 **21/21 / 19/19** |
| [UI/存档异常流程](Docs/Phase64/P64.D_UI_Save_Edges.md) | **5/5** 独立进程、**31/31** 动作、**79/79** runner 合同；增量 Editor 构建通过 |

后续专项证据：[委托来源](Docs/Phase64/P64.D_Delegate_Source_Context.md)、
[事件型 Guest](Docs/Phase64/P64.D_EventOnly_Runtime.md)、[C# 捕获赋值](Docs/Phase64/P64.D_Captured_Assignment.md)、
[UObject 授权](Docs/Phase64/P64.D_Reflection_Receiver.md)。这些验证不替代真实输入、视觉、设备和长稳验收；
[Android 边界](Docs/Phase64/P64.D_Android_Readiness.md)单独保留。

阶段状态与实现证据见 [Docs](Docs/)，开发规则见 [AGENTS.md](AGENTS.md)。

## 许可证

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH
LLVM-exception；`Source/ThirdParty/WAMR/upstream` 保留上游许可。Unreal Engine 不包含在本仓库中。
