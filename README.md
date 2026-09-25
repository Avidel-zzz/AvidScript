# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

面向 Unreal Engine 5.8 的 C# 脚本插件。C# 源码编译为 WebAssembly，在 UE Runtime 中执行，并通过生成的绑定访问引擎 API。

![C# 源码经编译器和 UE Runtime 调用 UObject](Docs/Assets/README/pipeline.svg)

## 示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`：每秒沿 X 轴移动 Actor 120 UE 单位。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 等待 0.25 秒，然后放大 Actor：

```csharp
LifetimeCancellation = AvidCancellationSource.Create();
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
```

`EndPlay` 调用 `Cancel()` 和 `Release()`，销毁 Actor 时不再执行等待后的代码。

![异步等待与对象销毁时的取消路径](Docs/Assets/README/async-lifecycle.svg)

## 快速开始

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [global.json](global.json) 指定的 .NET SDK。以下命令在 `Plugins/AvidScript` 下运行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 换成你的 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable** Cube。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。

只构建样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 样例

| 场景 | 代码 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 支持范围

- 主要开发环境为 UE 5.8 / Win64；真实游戏流程、真实多人联机和 Android/iOS 仍待验收。
- 编译器支持 C# 与 UE API 的子集，不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>`、`try/finally` 和受限同步 `catch/throw` 有语法边界。`catch/throw` 需显式启用 `-LanguageErrors bounded`，生成类型中的这一模式目前限 Win64 Development；`finally` 中不能 `await`。详见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)和[语言错误合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改脚本定义的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

代码位于 [Source/](Source/)（UE 插件）、[Tools/](Tools/)（C# 工具链）和 [Build/](Build/)（构建入口）。设计及验证记录见 [Docs/](Docs/)；贡献前请阅读 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
