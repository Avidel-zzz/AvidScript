# AvidScript

<img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript" width="600">

![Unreal Engine 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

UE 5.8 插件：编译 C# 子集为 WASM，在 UE 内运行，通过生成的绑定调用引擎 API。游戏进程不加载 CLR。

## Quick start

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在 `Plugins/AvidScript` 下执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 改为本机 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡里放置并选中一个 **Movable Cube**，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后 Play。Cube 会移动、旋转并放大。修改脚本后再次执行该菜单项。

只编译样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Examples

### Actor Tick

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每秒沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

### Await + teardown

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 等待 0.25 秒后放大 Actor；`EndPlay` 取消未完成的等待：

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

## Samples

| 功能 | 样例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Status

| 范围 | 当前状态 |
| --- | --- |
| 模板工程 · Win64 Editor / Development | 上述构建与 ActorLifecycle 流程已验证 |
| 异步、数组与泛型组合 | [示例](Fixtures/Phase66/IntegratedLanguageFlow.cs) 已通过 Win64 双后端自动化；[直接在 `try` 内等待下一帧](Fixtures/Phase66/DirectAwaitInTry.cs)尚不支持 |
| Android / iOS / Shipping | 尚未验收 |
| 真实多人游戏 | 尚未验收；提供 RPC 与属性复制样例 |

已知限制：

- 只支持项目实现的 C# 子集；不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 仅支持同一脚本实例内等待；`await` 与 `catch/finally` 的组合仍有限制。见 [Task 结果](Docs/Phase66/P66.C4_Task_Result_Contract.md)和 [异步异常](Docs/Phase66/P66.C5_Async_Language_Error_Contract.md)文档。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## Development

![C# 源码经编译器生成 WASM，并通过 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

[`Tools/`](Tools/) 是编译器，[`Source/`](Source/) 是 UE 插件，[`Build/`](Build/) 是构建脚本。设计与验证记录见 [`Docs/`](Docs/)，开发规则见 [`AGENTS.md`](AGENTS.md)。

运行 Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
