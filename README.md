# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

UE 5.8 的 C# 脚本插件。编译器将受支持的 C# 编译为 WASM；UE 插件加载 WASM，并通过生成的绑定调用引擎 API。UE 进程不加载 CLR。

当前适用范围：仓库自带的模板工程、Win64 Editor / Development。项目仍在开发；Android、iOS 和真实多人游戏尚未验收。

![C#、WASM 和 UE Runtime 的调用路径](Docs/Assets/README/pipeline.svg)

## 运行样例

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK。以下命令在 `Plugins/AvidScript` 目录执行。

安装仓库锁定的 Wasmtime 版本：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建模板工程的 Editor target。将 `UE_ROOT` 改为本机 UE 源码目录：

```powershell
$env:UE_ROOT = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable** Cube。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后进入 **Play**。样例会移动、旋转并放大 Cube。

只构建样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## C# 示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每秒沿 X 轴移动 Actor 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 在 `BeginPlay` 中等待 0.25 秒后放大 Actor；`EndPlay` 取消未完成的等待：

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

![DelayAsync 完成与 EndPlay 取消的执行路径](Docs/Assets/README/async-lifecycle.svg)

## 其他样例

| 场景 | 代码 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md)、[ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 限制

- 只支持部分 C# 语法和 UE API；不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 只能在同一脚本实例内等待，不能跨实例等待。见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- `-LanguageErrors bounded` 支持同步 `throw/catch` 和受支持的 `async Task<int>` 故障；`await` 后的 `catch/finally` 尚不能执行。见 [异常合同](Docs/Phase66/P66.C5_Async_Language_Error_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

运行 Guest 工具链测试：

```powershell
& (Join-Path $env:USERPROFILE '.dotnet\dotnet.exe') run `
  --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

目录：[Source/](Source/)（UE 插件）、[Tools/](Tools/)（C# 工具链）、[Build/](Build/)（构建脚本）、[Samples/](Samples/)（样例）、[Docs/](Docs/)（设计与验证记录）。仓库开发规则见 [AGENTS.md](AGENTS.md)。

## 许可证

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
