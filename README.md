# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 为 Unreal Engine 提供 C# 脚本支持。脚本编译为 WebAssembly，在 Wasmtime 或 WAMR 中运行，不依赖 CLR。

目前以 **UE 5.8 / Win64** 为主要开发环境，仍在开发中。C# 和 .NET API 尚未完整支持，使用前请查看 [Limitations](#limitations)。

[Installation](#installation) · [Quick start](#quick-start) · [Examples](#examples) · [Development](#development) · [Limitations](#limitations)

在 C# 中定义一个可被蓝图继承的 Actor：

```csharp
using AvidScript;

namespace AvidScriptSamples;

[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    [UFunction(BlueprintPure = true, Category = "Projectile")]
    public float GetLaunchSpeed()
    {
        return LaunchSpeed;
    }
}
```

生成 UE 类型并编译 Editor 后，蓝图可以读写 `LaunchSpeed`、调用 `GetLaunchSpeed()`。完整源码与构建步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## Installation

构建需要：

- Unreal Engine 5.8 源码版
- Visual Studio 2022 + UE C++ 工具链
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

从 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

将引擎路径和 `MyGame` 替换为自己的配置，然后编译 Editor：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>
<a id="getting-started"></a>

## Quick start

先用随插件提供的 [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 示例，让关卡中的 Cube 移动。将示例的 `Tick` 方法改为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是绑定此脚本的 Actor；速度为 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

1. 打开 Editor，在关卡中放置一个 Cube，将 **Mobility** 设为 **Movable**。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会沿 X 轴移动。

继续修改这个示例时，停止 Play，重新执行 Build And Bind，再次 Play。

<a id="samples"></a>

## Examples

| 示例 | 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick`、输入、碰撞 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | `[UClass]`、`[UProperty]`、`[UFunction]`、蓝图继承 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 等待定时器、异步加载资源 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |

## Development

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

编译器位于 `Tools/`，UE 插件模块位于 `Source/`。模块职责和依赖见[架构文档](Docs/Architecture/AvidScript_Module_Architecture.md)。

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>

## Limitations

| 范围 | 当前限制 |
| --- | --- |
| C# / .NET | 仅支持部分语法和 API，不能直接使用任意 NuGet 包；部分功能需要单独启用。见[语言支持](Docs/Phase66/P66.C_Language_Execution_Plan.md)。 |
| `async` / `await` | 泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`。见[异步异常](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。 |
| 取消 | C# 中暂不能读取 [`OperationCanceledException.CancellationToken`](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。 |
| 热重载 | 可更新方法体；新增 UE 类型、属性、函数或修改签名后，需要重新编译并重启 Editor。 |
| 平台 | 主要测试 UE 5.8 / Win64；Shipping、Android、iOS 和完整多人游戏流程仍待验收。 |

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
