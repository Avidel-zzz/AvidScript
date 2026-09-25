# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 UE 5.8 的 C# 脚本插件。工具链把受支持的 C# 子集编译为 WASM；UE 插件加载模块，通过生成的绑定访问引擎对象。运行时不嵌入 CLR。

**状态：开发中。** 当前上手流程针对仓库自带的 `AvidTPSTemplate.uproject` 和 Win64 Editor/Development。Android、iOS、真实多人联机与完整游戏流程仍待验收。

![C# 源码到 UE 调用的路径](Docs/Assets/README/pipeline.svg)

## Quick start

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK。以下命令均在 `Plugins/AvidScript` 目录执行。

安装锁定版本的 Wasmtime：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建模板工程的 Editor target，将 `UE_ROOT` 改为本机的 UE 源码目录：

```powershell
$env:UE_ROOT = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable** Cube。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后进入 **Play**。Cube 会移动、旋转并放大。

只编译该样例的 WASM，可直接运行：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Code

[`ActorLifecycleScript.cs`](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 通过 `UE.Self` 读写当前 Actor；下面的代码让它每秒沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[`LatentGameplayScript.cs`](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 在 `BeginPlay` 中等待 0.25 秒；`EndPlay` 取消尚未完成的等待：

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

![Delay 完成与 EndPlay 取消](Docs/Assets/README/async-lifecycle.svg)

## Samples

| 功能 | 代码与说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Current limits

- 编译器只接受已实现的 C# 语法和 UE API 子集；不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 支持受限的同源静态调用和 `await`；跨 Session 等待未实现。详见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `catch/throw` 需要 `-LanguageErrors bounded`。生成类型使用此模式目前限 Win64 Development。异步方法内抛错、`await` 后捕获、`finally` 中的 `await` 仍未形成可运行 WASM。详见 [语言错误合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改脚本定义的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## Development

运行 Guest 工具链测试（在插件根目录）：

```powershell
& (Join-Path $env:USERPROFILE '.dotnet\dotnet.exe') run `
  --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

源码入口：[Source/](Source/)（UE 插件）、[Tools/](Tools/)（编译器）、[Build/](Build/)（构建脚本）、[Samples/](Samples/)（样例）、[Docs/](Docs/)（设计与验证记录）。仓库开发规则见 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
