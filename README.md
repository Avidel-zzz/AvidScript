# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。工具链将支持的 C# 代码编译为 WebAssembly；UE 插件负责加载模块，并通过生成的绑定调用引擎 API。目前主要在 Win64 上开发和验证。

![从 C# 源码到 UE 对象调用的路径](Docs/Assets/README/pipeline.svg)

## 快速开始

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK。以下命令在 `Plugins/AvidScript` 目录执行。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 改为本机 UE 5.8 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个设为 **Movable** 的 Cube。选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。只编译该 C# 样例时运行：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 代码示例

[`ActorLifecycleScript.Tick`](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 在每帧读取并更新 Actor 的位置：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[`LatentGameplayScript`](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 在 `BeginPlay` 等待 UE 的 `Delay`，在 `EndPlay` 取消等待：

```csharp
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

![等待结束后继续执行，或在对象销毁时取消](Docs/Assets/README/async-lifecycle.svg)

## 样例

| 场景 | 代码与运行说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 当前限制

- 主要开发与验证平台是 UE 5.8 / Win64；Android、iOS 尚未验收。
- 编译器支持 C# 和生成的 UE API 子集，不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 目前只支持受限的同源静态调用与 `await`；跨 Session 等待尚未支持。见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `catch/throw` 需要显式启用 `-LanguageErrors bounded`。同一源码可包含独立的同步异常方法与 `Task<int>` 方法；异步方法内抛错及 `await` 后捕获仍未支持。`finally` 中不能 `await`；生成类型中的该模式目前限 Win64 Development。见 [语言错误合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改脚本定义的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。
- 样例和自动化测试尚不能代替真实游戏流程及真实多人联机验收。

## 开发

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

仓库入口：[Source/](Source/)（UE 插件）、[Tools/](Tools/)（C# 工具链）、[Build/](Build/)（构建脚本）、[Docs/](Docs/)（设计与验证记录）。仓库开发规则见 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
