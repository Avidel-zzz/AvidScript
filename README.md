# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 为 Unreal Engine 提供 C# 脚本支持。脚本编译为 WebAssembly，通过 UE 插件调用引擎 API，无需在运行时加载 .NET CLR。

开发中，支持 C# 子集。当前测试环境：**UE 5.8 / Win64 Editor、Development**。使用前请查看 [Limitations](#limitations)。

[Usage](#usage) · [Installation](#installation) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

## Usage

在 C# 中调用 UE API：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是挂载脚本的 Actor；沿 X 轴每秒移动 120 个 UE 单位
    FVector offset = new FVector(120.0f * deltaSeconds, 0.0f, 0.0f);
    UE.Self.AddActorWorldOffset(offset);
}
```

`avid_on_tick` 对应 UE 的 Tick，`deltaSeconds` 单位为秒。安装后，用上面的函数替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`，保留文件其余内容。

<a id="快速开始"></a>
<a id="安装"></a>
<a id="quick-start"></a>

## Installation

需要 UE **5.8 源码版**、Visual Studio 2022（UE C++ 开发工具）、PowerShell 7、.NET SDK [**8.0.416**](global.json)。

在 UE 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建 Editor，以下以 `AvidTPSTemplate` 为例。替换引擎路径、`.uproject` 文件名和 Editor target：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开工程，放置并选中一个 Cube，将 Mobility 设为 **Movable**。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。默认示例会移动、旋转和缩放 Cube。

修改脚本后，再次执行同一菜单命令编译并绑定。

<a id="示例"></a>

## Examples

| 示例 | API / 用途 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入、碰撞回调 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `async / await`、延迟、取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Client / Server / Multicast RPC 调用 |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步与 RepNotify |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 与存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目中的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 用 C# 声明 Actor、Component、Subsystem |

<details>
<summary>示例：await 延迟与取消</summary>

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 节选：等待 0.25 秒后缩放 Actor，EndPlay 时取消等待。该示例需使用 [LatentGameplay 的构建配置](Samples/CSharp/LatentGameplay/README.md)。

```csharp
private static AvidCancellationSource LifetimeCancellation;

[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    LifetimeCancellation = AvidCancellationSource.Create();

    await UKismetSystemLibrary.DelayAsync(0.25f)
        .WithCancellation(LifetimeCancellation.Token);

    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}

[UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
public static void EndPlay()
{
    LifetimeCancellation.Cancel();
    LifetimeCancellation.Release();
}
```

</details>

<a id="当前边界"></a>
<a id="已知限制"></a>
<a id="支持范围"></a>

## Limitations

- **C# / .NET**：支持的语法见[语言支持文档](Docs/Phase66/P66.C_Language_Execution_Plan.md)。不能直接运行任意 .NET 程序或 NuGet 包。
- **async / await**：`catch`、`finally` 内不能 `await`；异步异常处理需显式启用。配置与支持范围见 [UE 异步等待](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)、[Task 取消](Docs/Phase66/P66.C7_Async_Cancellation_Language_Contract.md)。
- **UE 类型声明**：修改 C# 声明的 `UClass`、`UProperty`、`UFunction` 后，需要重新构建并重启 Editor。
- **平台**：Android、iOS、Shipping 和真实多人游戏尚未验收。

<a id="构建与测试"></a>
<a id="开发"></a>
<a id="build-and-test"></a>

## Development

在插件目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<a id="项目结构"></a>
<a id="internals"></a>

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

```text
Source/    UE 插件模块
Tools/     C# 编译器与 WASM 生成工具
Build/     构建与验证脚本
Samples/   示例脚本
Docs/      使用说明与设计文档
```

图中的 Guest IR 是编译器的中间表示；ObjectHandle 是脚本访问 UE 对象时使用的句柄。

<a id="许可"></a>

## License

[MIT](LICENSE)。第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
