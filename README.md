<div align="center">

# AvidScript

**面向 Unreal Engine 的现代 C# + WebAssembly 游戏脚本框架**

<p>
  <img alt="Unreal Engine 5.8" src="https://img.shields.io/badge/Unreal%20Engine-5.8-0E1128?logo=unrealengine&logoColor=white">
  <img alt="C# Guest" src="https://img.shields.io/badge/Guest-C%23-512BD4?logo=dotnet&logoColor=white">
  <img alt="WebAssembly" src="https://img.shields.io/badge/Target-WebAssembly-654FF0?logo=webassembly&logoColor=white">
  <img alt="Wasmtime 45" src="https://img.shields.io/badge/VM-Wasmtime%2045-2B6CB0">
  <img alt="Win64 and Android arm64 AOT" src="https://img.shields.io/badge/Platform-Win64%20%2B%20Android%20arm64%20AOT-0078D4?logo=windows&logoColor=white">
  <img alt="Phase 64 Active" src="https://img.shields.io/badge/Status-Phase%2064%20Active-2B6CB0">
  <img alt="Automation Baseline 433/433" src="https://img.shields.io/badge/Baseline-433%2F433-26A269">
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/License-MIT-2E8B57"></a>
</p>

AvidScript 将 C# 编译为轻量 WASM Guest，通过 Reflection 生成的 Binding 接入 UE 生命周期、
项目 API、网络与异步流程。Win64 主后端使用 Wasmtime 45，WAMR 保留为兼容与移动端候选。

</div>

> [!IMPORTANT]
> 当前版本为 **0.1.0 开发者预览**，主线验证环境是 **UE5.8 源码版 + Win64
> Development/Shipping + Wasmtime 45**。两种 Win64 配置的 BuildCookRun 均已通过；Android arm64
> 交叉 AOT 发布已验证，Android UBT/真机与 iOS 仍待正式验收。

## 已实现

| 领域 | 已验证能力 |
| --- | --- |
| 生命周期与启动 | `BeginPlay/Tick/EndPlay`、Timer、Overlap、Gameplay Event；Startup Scenario 自动挂载、原子回滚与 teardown |
| C# 游戏样例 | `PickupRush` 计时、收集、复活和胜负逻辑；同源 Editor、Win64 Development/Shipping 包内行为验证通过 |
| 生成式 UE API | Reflection/Profile 生成 `UFUNCTION`、`UPROPERTY`、Interface 与项目自定义 API；支持 UObject、向量/变换、固定 `USTRUCT`、名称/字符串、一维 `TArray<T>` |
| C# 定义 UE 类型 | Actor、Component、World/GameInstance Subsystem，含继承、override、属性、函数与默认参数 |
| 异步与委托 | 受控 `async/await`、Delay/NextTick、异步加载、Latent、AsyncAction；单播/多播、强类型 `return/ref/out` 与主动调用 |
| Blueprint 与网络 | callable/event 双向交互；Server/Client/NetMulticast RPC、属性复制与 RepNotify，dedicated/listen 多进程验证 |
| 热重载与安全 | 方法体事务式替换与回滚；ObjectHandle、Session 隔离、执行预算、后台恢复、低内存回收及 teardown 自动取消 |
| Cook 与发布 | 内容寻址模块、多平台 catalog、无头 Release、Generated Type 预编译；Win64 Development/Shipping BuildCookRun 与回执校验 |
| VM 后端 | Wasmtime 45 Win64 JIT/AOT；Android arm64 脚本及 Generated Type 交叉 AOT；WAMR 兼容后端 |
| 构建与 IDE | 增量缓存、persistent Worker、`.slnx`/WASI 工作区、离线源码索引与 Visual Studio/Rider/VS Code 启动 |
| 调试与 Profiler | 源码映射、跨层调用栈、PIE 目标、受控同步断点/步进、只读变量；UE Trace、热点与 JSON 导出 |

**最近落地：** [PickupRush](Samples/CSharp/PickupRush/README.md) 已完成 Win64 包内事件与胜利状态验证；
[集成修复](Docs/Phase64/P64_Integration_Review.md) 补齐真实启动回滚、热重载事件隔离及冷启动预编译包校验。
[Android 就绪工具](Docs/Phase64/P64.D_Android_Readiness.md) 提供 SDK 预检、平台回执和绑定包身份的设备运行入口。
Android APK/真机及人工游玩仍待验收，不能用交叉 AOT 或自动事件注入替代。

## C# 游戏脚本

下面的 typed API 来自项目 Reflection，不是为单个 API 手写的 VM wrapper：

```csharp
using System.Runtime.InteropServices;

namespace AvidScript;

public static class GameScript
{
    private static AActor Projectile;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        AAvidScriptTypedTestActor self = UE.Self;
        self.ApplyGameplayValue(4.0f);

        Projectile = UE.SpawnActor(
            ProjectClasses.Projectile,
            FTransform.Identity);

        await AvidContinuations.NextTickAsync();
        Projectile.SetActorScale3D(new FVector(1.05f, 1.0f, 1.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (Projectile.HasHandle)
        {
            Projectile.AddActorWorldOffset(
                new FVector(100.0f * deltaSeconds, 0.0f, 0.0f));
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        if (Projectile.HasHandle)
        {
            UE.DestroyActor(Projectile);
            Projectile = default;
        }
    }
}
```

完整样例见 [PickupRush](Samples/CSharp/PickupRush/README.md)、
[TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) 与
[ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。
Editor 可生成含 `.slnx`、`.editorconfig`、WASI `.csproj`、固定 SDK 和构建 profile 的 C# 工作区；
刷新默认保留用户文件，只更新 Reflection typed facade 与不含绝对路径的离线源码索引。IDE 启动服务支持
系统默认关联、Visual Studio、Rider 和 VS Code，并已接入 Editor Tools 菜单。

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

以下数据来自 UE5.8 Win64 Development、同机冻结 workload。比率小于 `1.0x` 表示
AvidScript 更快；这些结论只适用于对应 workload，不外推到所有脚本代码。

![Prepared Reflection 性能对比](Docs/Assets/README/phase57-prepared-reflection-performance.svg)

| UE 交互场景 | AvidScript P50 | Puerts Reflection P50 | 比率 |
| --- | ---: | ---: | ---: |
| Scalar UFUNCTION | `54.57 ns` | `106.15 ns` | **`0.514x`** |
| Property get/set | `68.14 ns` | `103.01 ns` | **`0.661x`** |
| FVector value | `66.49 ns` | `1193.10 ns` | **`0.056x`** |
| UObject roundtrip | `69.60 ns` | `130.90 ns` | **`0.532x`** |

Phase 56 完整游戏 workload 中，Small gameplay、Dense gameplay 与 Lifecycle callback 的
P50 比率分别为 **`0.469x`、`0.513x`、`0.391x`**。编译器托管数组区域在 `N>=64`
的冻结 workload 中领先 Puerts TArray **17.47x-20.04x**。

增量构建的无修改热路径为零编译调用，5 轮中位数 `806 ms`。

P60 的新增 UE 交互路径均在 warm path 禁止名称反射查找，并使用固定 20 组样本：

| P60 路径 | UE/原生 P50 | AvidScript P50 | 比率或预算 |
| --- | ---: | ---: | ---: |
| Interface | `0.151 us` | `0.387 us` | `2.56x` |
| Delegate 主动调用 | `0.553 us` | `0.726 us` | `1.31x` |
| Blueprint callable | `0.641 us` | `0.779 us` | `1.22x` |
| AsyncAction Session 生命周期 | 不适用 | `4.308 us` | `< 250 us` |

这些数字用于同一 UE 构建内的回归门禁，不等同于完整 WASM crossing 或竞品对比。

纯执行层仍未宣布绝对领先：12-kernel Wasmtime/V8 suite 的 P50/P95 几何均值为
`0.9800x / 1.0006x`，P95 领导力门禁仍未关闭。

完整口径与机器可读证据：

- [Prepared Reflection 证据](Docs/Phase57/P57.11B1_Recursive_Fixed_Struct_Codec_Evidence.json)
- [编译器托管数组区域报告](Docs/Phase57/P57.11D_Compiler_Managed_Array_Region.md)
- [Phase 56 游戏 workload 报告](Docs/Phase56/P56.5_Fused_Call_Frame_Implementation_Report.md)
- [Wasmtime Cranelift Speed 报告](Docs/Phase57/P57.13_Cranelift_Speed_Profile.md)
- [Phase 60 性能矩阵与 Gate](Docs/Phase60/P60.D_Performance_And_Gate.md)
- [Phase 61 集中 Gate](Docs/Phase61/P61.E_Integration_Gate.md)

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
| [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) | Overlap、Gameplay Event、持久状态与热重载 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | C# 调用项目 Server、Client 与 NetMulticast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | replicated property、authority setter 与 Push Model |
| [NetworkTopology](Samples/CSharp/NetworkTopology/README.md) | dedicated/listen 多进程网络闭环 |

## 当前边界

- 生成范围由 Binding Profile 与现有 ABI/codec 决定，还不是完整 UE 类型系统；
- C# `event +=` 与 lambda/closure 语法糖尚未实现；当前使用生成的显式 bind/subscribe 与 `ExecuteX/BroadcastX`；
- 容器当前聚焦一维 `TArray<T>`；nested array、`TSet`、`TMap` 与 `FText` 尚未完整支持；
- C# 前端提供受控游戏脚本子集，不包含完整 .NET Runtime、任意 awaiter 或异常系统；
- 反射结构变化仍需 no-clean UBT 并重启 Editor，方法体变化可自动热重载；
- Generated Type 预编译发布与 Win64 Development/Shipping 打包启动已验收；Android arm64 交叉 AOT 已验证，但移动端 UBT、打包和真机尚未验收；Session 可隔离受控 WASM 故障，但原生 C++/DLL 崩溃不属于进程沙箱能力；
- 正式竞品性能矩阵目前覆盖 Puerts V8，尚未同口径覆盖 UnLua 与 AngelScript。

## 路线图

1. **P62/P63 已收尾**：Win64 Cook/Shipping、多平台 catalog、Android 交叉 AOT 与移动 Runtime 生命周期；
2. **P64 进行中**：游戏 Demo、跨平台运行工具与集中回归；移动真机验收仍是开放项；
3. **后续发布工程**：安装/升级、兼容与诊断，逐项补齐类型、移动端、真实游戏及性能验收缺口。

路线图只表示工程顺序，不代表对应能力已经可用。

## 验证

最近一次完整基线为 **AvidScript Automation `433/433`、.NET `284/284` 通过**。
Phase 63 的 clean detached architecture、UE5.8 no-clean UBT、Android arm64 交叉 AOT、
发布/Cook/回执合同与移动 Runtime 生命周期均已通过，并已完成正式 attestation 与 close。
证据见 [Phase 63 Gate](Docs/Phase63/P63_Gate_Summary.json) 与 [收尾记录](Docs/Phase63/P63_Closeout.md)。
Phase 64 的 `PickupRush` Editor、Development/Shipping package 均验证 5/5 事件、胜利状态与零丢弃回调；
两种打包配置的精确回执分别为 21/21、19/19。该批次证据尚未计入完整 Automation 基线。
Android 就绪入口及当前未运行项见 [P64.D 验证记录](Docs/Phase64/P64.D_Android_Readiness.md)。
集中修复已通过聚焦 Automation `9/9`、Android runner `24/24` 和修正版 PickupRush Editor 胜利状态校验；
修正版完整 Gate 与 Win64 打包重验仍待完成，不沿用旧包证据。

阶段状态与实现证据见 [Docs](Docs/)，开发规则见 [AGENTS.md](AGENTS.md)。

## 许可证

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH
LLVM-exception；`Source/ThirdParty/WAMR/upstream` 保留上游许可。Unreal Engine 不包含在本仓库中。
