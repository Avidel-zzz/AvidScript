# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。脚本编译为 WebAssembly，可调用 UE API，运行时不依赖 .NET CLR。

> [!NOTE]
> 开发预览。目前支持 C# 子集，测试环境为 UE 5.8 / Win64 Editor、Development。使用前请阅读[已知限制](#limitations)。

[Usage](#usage) · [Installation](#installation) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

## Usage

下面的脚本让 Actor 每秒沿 X 轴移动 120 个 UE 单位：

```csharp
using System.Runtime.InteropServices;

namespace AvidScript;

public static class ActorLifecycleScript
{
    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay() { }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        FVector offset = new FVector(120.0f * deltaSeconds, 0.0f, 0.0f);
        UE.Self.AddActorWorldOffset(offset);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() { }
}
```

`UE.Self` 是挂载脚本的 Actor；`EntryPoint` 指定 UE 调用的脚本入口，`avid_on_tick` 每帧执行一次。安装后，可用以上内容替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 并通过菜单编译运行。

<a id="快速开始"></a>
<a id="安装"></a>
<a id="quick-start"></a>

## Installation

需要 UE **5.8 源码版**、Visual Studio 2022（UE C++ 工具）、PowerShell 7 和 .NET SDK [**8.0.416**](global.json)。

在 UE 工程目录下克隆插件：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
```

安装 Wasmtime 并构建 Editor。以下以 `AvidTPSTemplate` 为例，其他工程需替换 `.uproject` 文件名和 Editor target：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$ueRoot = 'C:\UnrealEngine' # 改为本机 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开工程，放置一个 Cube，将 Mobility 设为 **Movable**，选中它。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。默认示例会移动、旋转和缩放 Cube。

修改 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs` 后，再次执行该菜单命令。

<a id="示例"></a>

## Examples

| 示例 | 演示内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入和碰撞回调 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 等待 0.25 秒后缩放 Actor，结束时取消等待 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 从脚本发送客户端 / 服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，并在 `RepNotify` 中响应变化 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 与存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目中的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 声明 Actor、Component、Subsystem |

<details>
<summary>查看 async / await 示例</summary>

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 节选：

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

- **C# / .NET**：不支持完整 C#，不能直接运行任意 .NET 程序或 NuGet 包。
- **异步**：`Task<int>` 只能在同一脚本实例内等待；`catch`、`finally` 内不支持 `await`。异常处理需要下方的预览开关。
- **UE 类型声明**：修改 C# 声明的 `UClass`、`UProperty`、`UFunction` 后，需要重新构建并重启 Editor。
- **平台与发布**：Android、iOS、Shipping、真实多人游戏尚未验收。

<details>
<summary>异步异常处理的预览开关</summary>

- `try` 中等待 UE 异步操作：见 [Direct await 配置与限制](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)。
- 捕获取消异常、使用 `throw;` 重新抛出：需显式启用 [`-AsyncCancellationFlow`](Docs/Phase66/P66.C7_Async_Cancellation_Language_Contract.md)。该模式下，未处理的 Task 取消会报告异常类型和源码位置。

</details>

<a id="构建与测试"></a>
<a id="开发"></a>
<a id="build-and-test"></a>

## Development

从插件目录执行：

```powershell
# 编译 ActorLifecycle 示例，生成 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<a id="项目结构"></a>
<a id="internals"></a>

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

```text
Source/    UE 插件模块
Tools/     C# 编译器、中间表示、WASM 生成工具
Build/     构建与验证脚本
Samples/   示例脚本
Docs/      使用说明、设计与测试记录
```

编译器支持的语法及测试记录见[语言实现文档](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

<a id="许可"></a>

## License

[MIT](LICENSE)。第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
