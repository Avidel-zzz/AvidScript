# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。编译器把受支持的 C# 代码编译成 WebAssembly；UE 插件加载模块，通过生成的绑定访问引擎 API。游戏进程不需要 CLR。

<img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript：C#、WebAssembly 与 Unreal Engine" width="500">

## Requirements

- UE 5.8 源码版
- Visual Studio 2022，安装 UE C++ 工作负载
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

目前以仓库自带模板工程的 **Win64 Editor / Development** 为验证目标。

## Run the sample

在 `Plugins/AvidScript` 目录安装 Wasmtime 依赖并构建 Editor：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 改为本机 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable Cube**。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 Play。Cube 会移动、旋转、放大。修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，重新执行第 2 步。

只生成样例 WASM，不启动 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Code

`Tick` 使用生成的 UE 绑定移动 Actor，速度为每秒 120 UE 单位。代码来自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 在 `BeginPlay` 中等待 0.25 秒；`EndPlay` 取消尚未完成的等待：

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

| 功能 | 入口 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Support

| 范围 | 状态 |
| --- | --- |
| 模板工程，Win64 Editor / Development | 构建与 ActorLifecycle 样例已验证 |
| 异步、数组与泛型组合 | [集成样例](Fixtures/Phase66/IntegratedLanguageFlow.cs)通过 Win64 Wasmtime/WAMR 自动化 |
| Android / iOS / Shipping | 尚未验收 |
| 真实多人游戏 | 尚未验收；仓库提供 RPC 与属性复制样例 |

当前限制：

- 编译器只支持项目实现的 C# 子集，不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 只支持同一脚本实例内等待，见 [Task 结果](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- [直接在 `try` 内等待下一帧](Fixtures/Phase66/DirectAwaitInTry.cs)尚不支持，见[异步异常](Docs/Phase66/P66.C5_Async_Language_Error_Contract.md)。
- C# 声明的 `UClass`、`UProperty` 或 `UFunction` 变更后，需要重新构建并重启 Editor。

## Development

![C# 源码经编译器生成 WASM，并通过 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

源码入口：[Tools/](Tools/) 是 C# 前端与 WASM 生成工具，[Source/](Source/) 是 UE 插件模块；构建脚本在 [Build/](Build/)，设计和验证记录在 [Docs/](Docs/)。

运行 Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

开发约定见 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
