# AvidScript

![AvidScript: C# scripts for Unreal Engine](Docs/Assets/README/avidscript-hero.svg)

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# → WASM 脚本插件。脚本在构建时编译，运行时由 UE 插件加载。现阶段主要在 Win64 开发和测试。

## 快速开始

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在 `Plugins/AvidScript` 目录运行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 改成你的 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放一个 **Movable** Cube 并选中它。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。对应源码是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

如果只想编译这个样例的 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## C# 脚本示例

`UE.Self` 是当前绑定的 Actor。下面摘自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)；每帧沿 X 轴移动 120 UE 单位/秒：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

也可以在 `BeginPlay` 中等待 UE 的异步操作。这个片段简化自 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)；`EndPlay` 负责取消等待：

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

## 更多示例

| 功能 | 样例 |
| --- | --- |
| 输入、碰撞、Actor 生命周期 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent 与取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目里的 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 工作原理

![C# 源码、WASM 与 UE Runtime 的关系](Docs/Assets/README/pipeline.svg)

Roslyn 只参与构建。运行时没有 CLR；WASM 通过生成的绑定和 `ObjectHandle` 访问 UE 对象。

## 当前状态与限制

- Win64 Editor、打包样例和独立进程网络样例有自动化测试；真实游戏流程、真实多人联机以及 Android/iOS 仍待验收。
- 这是 C# 子集，不是通用 .NET 运行时。普通 .NET 项目和 NuGet 包不能直接放进来运行。
- `Task<int>` 支持 `await`、赋值给已有的 `int` 局部变量或脚本静态字段、有限的别名；不支持任务变量重赋值和其他 `Task<T>`。[具体规则](Docs/Phase66/P66.C4_Task_Result_Contract.md)
- 同步 `try/finally` 可用；正常构建仍不接受 `catch`、`throw`，也不支持在 `finally` 中 `await`。[具体规则](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)
- 改动 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

从插件根目录运行 Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

[Source/](Source/) 是 UE 插件，[Tools/](Tools/) 是 C# 工具链，[Build/](Build/) 是构建脚本。设计和验证记录在 [Docs/](Docs/)；修改代码前先读 [AGENTS.md](AGENTS.md)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
