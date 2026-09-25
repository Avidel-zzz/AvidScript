# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

C# → WebAssembly → Unreal Engine 5.8。AvidScript 编译支持的 C# 子集，UE 插件加载 WASM 并通过生成的绑定调用引擎 API。运行时不需要 CLR。

**Status:** 开发中。以下样例和命令针对仓库自带的模板工程及 **Win64 Editor / Development**；Android、iOS 和真实多人游戏尚未验收。

![C# 到 Unreal Engine 的编译与运行路径](Docs/Assets/README/pipeline.svg)

## Example

### Actor Tick

[`ActorLifecycleScript.cs`](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 读取并更新当前 Actor 的位置。`deltaSeconds` 为 1 时，X 坐标增加 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

### Await and cancel

[`LatentGameplayScript.cs`](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 等待 0.25 秒后放大 Actor；`EndPlay` 会取消还没完成的等待：

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

![等待完成与 EndPlay 取消](Docs/Assets/README/async-lifecycle.svg)

## Build and run

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK。以下命令在 `Plugins/AvidScript` 目录执行。

1. 安装锁定版本的 Wasmtime：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建模板工程的 Editor target。把 `UE_ROOT` 改成本机 UE 源码目录：

   ```powershell
   $env:UE_ROOT = 'C:\UnrealEngine'
   $project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
   & (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
     AvidTPSTemplateEditor Win64 Development "-Project=$project" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable** Cube。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，进入 **Play**。样例会移动、旋转并放大这个 Cube。

只生成样例 WASM、不启动 Editor 时，运行：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Samples

| 要做什么 | 从这里看代码 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 在 C# 中声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Current limitations

- 编译器只实现了部分 C# 语法和 UE API；不能把任意 .NET 项目或 NuGet 包直接作为脚本运行。
- `Task<int>` 可以在同一脚本 Session 内调用并等待；把 Task 交给另一个 Session 等待还不支持。见 [Task 结果说明](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- `-LanguageErrors bounded` 支持同步 `throw/catch`；在受支持的 `async Task<int>` 中，`throw new InvalidOperationException()` 能使任务失败。`await` 后的 `catch` 和异步 `finally` 仍不支持。见 [同步异常](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)和 [异步异常](Docs/Phase66/P66.C5_Async_Language_Error_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## Development

运行 Guest 工具链测试：

```powershell
& (Join-Path $env:USERPROFILE '.dotnet\dotnet.exe') run `
  --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

源码目录：[Source/](Source/)（UE 插件）、[Tools/](Tools/)（C# 工具链）、[Build/](Build/)（构建脚本）、[Samples/](Samples/)（样例）、[Docs/](Docs/)（设计和验证记录）。贡献与开发规则见 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
