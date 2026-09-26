# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，由 UE 插件加载执行；运行时不托管 .NET CLR。

**Status:** 开发中。当前测试环境为 UE 5.8 / Win64 Editor、Development，支持的 C# 语法见[语言实现文档](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

[Installation](#installation) · [Usage](#usage) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

<a id="快速开始"></a>
<a id="安装"></a>
<a id="quick-start"></a>

## Installation

依赖：

- Unreal Engine 5.8 源码版
- Visual Studio 2022，安装 UE C++ 开发工具
- PowerShell 7
- .NET SDK **8.0.416**（[global.json](global.json)）

在 UE 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建 Editor。替换下面的引擎路径、工程文件名和 target：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

## Usage

1. 打开工程，在关卡中放置 Cube，将 Mobility 设为 **Movable**，选中它。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。仓库自带的示例会移动、旋转和缩放 Cube。

脚本文件：[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。修改后再次执行同一菜单命令，重新编译并绑定。

以下是只保留移动逻辑的版本。替换文件中的 `ActorLifecycleScript` 类，保留原有 `using`、`namespace` 和其余类型定义：

```csharp
public static class ActorLifecycleScript
{
    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay() { }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        // 沿 X 轴每秒移动 120 个 UE 单位
        FVector offset = new FVector(120.0f * deltaSeconds, 0.0f, 0.0f);
        UE.Self.AddActorWorldOffset(offset);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() { }
}
```

- `UE.Self`：挂载脚本的 Actor。
- `EntryPoint`：导出给 UE 调用的函数名。这里分别对应 BeginPlay、Tick 和 EndPlay。
- `deltaSeconds`：当前帧的时间间隔，单位为秒。

<a id="示例"></a>

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | Actor 生命周期、输入、碰撞回调 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 延迟执行与取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Client / Server / Multicast RPC 调用 |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步与 RepNotify |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 与存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 项目 C++ API 绑定 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 用 C# 声明 Actor、Component、Subsystem |

<details>
<summary>async / await：延迟 0.25 秒后缩放 Actor</summary>

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 节选。该示例使用自己的构建配置，运行方法见 [LatentGameplay](Samples/CSharp/LatentGameplay/README.md)。

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
    // Actor 结束运行时，取消尚未完成的等待并释放取消源
    LifetimeCancellation.Cancel();
    LifetimeCancellation.Release();
}
```

</details>

<a id="当前边界"></a>
<a id="已知限制"></a>
<a id="支持范围"></a>

## Limitations

- 仅支持 C# 子集，不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 可在同一脚本实例内重复等待。`catch`、`finally` 内暂不支持 `await`。
- 异步异常处理默认关闭。在 `try` 中 [await UE 异步操作](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)、[捕获 Task 取消异常](Docs/Phase66/P66.C7_Async_Cancellation_Language_Contract.md) 时，需按文档启用编译选项。
- 修改 C# 声明的 `UClass`、`UProperty`、`UFunction` 后，需要重新构建并重启 Editor。
- Android、iOS、Shipping 和真实多人游戏尚未验收。

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
Tools/     C# 编译器、Guest IR、WASM 生成工具
Build/     构建与验证脚本
Samples/   示例脚本
Docs/      使用说明与设计文档
```

Guest IR 是编译器使用的中间表示；构建工具将它转换为 WASM，UE 插件负责加载和执行。

<a id="许可"></a>

## License

[MIT](LICENSE)。第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
