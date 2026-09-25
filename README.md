# AvidScript

<img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript" width="720">

![Unreal Engine 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 UE 5.8 的 C# 脚本插件。工具链将受支持的 C# 代码编译成 WebAssembly，UE 运行时通过生成的绑定调用引擎 API，无需在游戏进程中加载 CLR。

目前主要验证环境是仓库自带模板工程的 **Win64 Editor / Development**。其他平台和 Shipping 状态见[支持范围](#支持范围)。

## 快速运行

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在 `Plugins/AvidScript` 目录执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 改为本机 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable Cube**。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后 Play。Cube 会移动、旋转并放大。修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，重新执行该菜单项。

只构建样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 代码示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每秒沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 中的 `BeginPlay` 等待 0.25 秒后放大 Actor。`EndPlay` 取消未完成的等待：

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

## 更多样例

| 场景 | 源码与说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 工作方式

![C# 源码经编译器生成 WASM，并通过 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

Roslyn 解析 C#；编译工具生成 WASM；UE 插件加载模块，并用生成的绑定访问引擎对象。实现入口见 [Tools/](Tools/)（编译器）和 [Source/](Source/)（UE 插件）。

## 支持范围

| 环境 | 当前状态 |
| --- | --- |
| 模板工程 · Win64 Editor / Development | 可按上面的步骤构建和运行 |
| Android / iOS / Shipping | 尚未验收 |
| 真实多人游戏 | 尚未验收；仓库包含 RPC 与属性复制样例 |

- 编译器仅支持项目实现的 C# 子集；不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 目前只支持同一脚本实例内等待，详见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- `await` 后的 `catch/finally` 仅支持受限的 `Task<int>` 场景，需要显式传入 `-LanguageErrors bounded -AsyncExceptionFlow`；过滤器和处理块内再次 `await` 尚不支持。详见 [异步异常合同](Docs/Phase66/P66.C5_Async_Language_Error_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

运行 Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

构建脚本在 [Build/](Build/)，设计与验证记录在 [Docs/](Docs/)，仓库开发约定见 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
