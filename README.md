# AvidScript

![AvidScript](Docs/Assets/README/avidscript-hero.svg)

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

UE 5.8 的 C# 脚本插件：将受支持的 C# 编译为 WebAssembly，在 UE 中通过生成的绑定调用引擎 API。运行游戏时不需要 CLR。

![从 C# 源码到 UE 对象的执行路径](Docs/Assets/README/pipeline.svg)

## Quick start

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、[.NET SDK 8.0.416](global.json)。

在 `Plugins/AvidScript` 目录安装 Wasmtime 依赖，并构建模板工程：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 改成你的 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放一个 **Movable** Cube 并选中。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后 Play。预期结果：Cube 移动、旋转并放大。

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，重新执行该菜单项。只构建 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Example

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 每秒沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 等待 0.25 秒后放大 Actor；`EndPlay` 会取消未完成的等待：

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

![等待完成或 Actor 结束时的取消路径](Docs/Assets/README/async-lifecycle.svg)

## Samples

| 场景 | 源码与说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Support

| 环境 | 状态 |
| --- | --- |
| 模板工程 · Win64 Editor / Development | 按上面的步骤构建和运行 |
| Android / iOS / Shipping | 尚未验收 |
| 真实多人游戏 | 尚未验收；仓库提供 RPC 与属性复制样例 |

已知限制：

- 只编译项目支持的 C# 子集；不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 只能在同一脚本实例内等待。[详细限制](Docs/Phase66/P66.C4_Task_Result_Contract.md)
- `await` 后的 `catch/finally` 仅支持受限 `Task<int>` 场景，需用 `-LanguageErrors bounded -AsyncExceptionFlow` 显式开启；过滤器和处理块内再次 `await` 暂不支持。[详细限制](Docs/Phase66/P66.C5_Async_Language_Error_Contract.md)
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## Development

运行 C# Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

代码在 [Source/](Source/) 和 [Tools/](Tools/)；构建脚本在 [Build/](Build/)，设计与验证记录在 [Docs/](Docs/)。参与开发前请阅读 [AGENTS.md](AGENTS.md)。

## License

原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
