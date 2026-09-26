# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。使用 Roslyn 将 C# 编译为 WebAssembly，通过 Wasmtime / WAMR 运行，不在 UE 中嵌入 .NET 运行时。

> **Experimental** · UE 5.8 · Win64 Editor / Development。仅支持部分 C# 和 .NET API，见 [Limitations](#known-limitations)。

[Quick start](#quick-start) · [Examples](#examples) · [Limitations](#known-limitations) · [Development](#development) · [License](#license)

## Usage

使用 `[UClass]`、`[UProperty]` 和 `[UFunction]` 定义 UE 类型：

```csharp
using AvidScript;

namespace AvidScriptSamples;

[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    // 蓝图可读写的属性。
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    // 蓝图可调用的函数；等待下一帧后修改属性。
    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public async void SetLaunchSpeedNextTick(float speed)
    {
        await AvidContinuations.NextTickAsync();
        LaunchSpeed = speed;
    }
}
```

`Projectile` 可作为蓝图父类。调用 `SetLaunchSpeedNextTick(900.0f)`，下一帧的 `LaunchSpeed` 为 `900`。

示例节选自 [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)。新增 UE 类型或修改反射声明后，需要重新编译并重启 Editor；[方法体修改支持热重载](Samples/CSharp/ScriptDefinedTypes/README.md)。

<a id="installation"></a>
<a id="quick-start"></a>

## Quick start

### Requirements

- Unreal Engine 5.8 源码版
- Visual Studio 2022 + UE C++ 开发工具
- PowerShell 7
- .NET SDK [8.0.416](global.json)

### Install

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

### Build

在 `Plugins/AvidScript` 下执行。将 `MyGame` 替换为工程名，`$ueRoot` 替换为引擎路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

### Run

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

编辑 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick`，让 Cube 以 120 cm/s 沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是当前绑定的 Actor，deltaSeconds 是帧间隔（秒）。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

保存后重新执行 **Build And Bind**。

<a id="samples"></a>

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 移动 Actor，处理 `BeginPlay` / `Tick` / `EndPlay`、输入与碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 等待后缩放 Actor，在 `EndPlay` 时取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 调用 Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，用 `RepNotify` 处理客户端更新 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 更新 UI、读写存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>
<a id="known-limitations"></a>

## Limitations

目前用于 Win64 开发与测试，尚未完成 Shipping、Android、iOS 和真实多人游戏验收。

| 项目 | 当前限制 |
| --- | --- |
| C# | 支持普通类的实例字段初始化与构造函数链；暂不支持从 C# 编译执行[静态对象字段](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)。 |
| .NET | 仅支持部分 API，不能直接使用任意 NuGet 包。 |
| `async` / `await` | 带返回值的 Task 仅支持 `Task<int>`；不支持在 `catch` / `finally` 中 `await`。 |
| 异常 | 部分同步方法、getter/setter 支持显式 `throw`，需启用[实验性构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)；尚不支持同步异常向异步方法传播。 |
| 热重载 | 支持方法体修改；新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。 |

[C# 支持记录](Docs/Phase66/P66.C_Language_Execution_Plan.md) · [Task 异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)

## Development

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

### Architecture

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

Roslyn 和 Guest IR（编译器中间表示）用于构建。UE 运行时加载 WASM，通过 `ObjectHandle` 访问 UE 对象；脚本不直接持有 `UObject*`。

```text
Source/    UE 插件模块
Tools/     C# 编译器与代码生成工具
Build/     构建与测试脚本
Samples/   示例项目
Docs/      设计文档与测试记录
```

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
