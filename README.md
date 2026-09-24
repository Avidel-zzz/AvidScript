# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。工具链将受支持的 C# 编译为 WebAssembly；UE 在运行时加载 WASM，通过生成的绑定调用 UE API，不需要在游戏进程中加载 CLR。目前主要在 Win64 上开发和验证。

![C# 源码到 UE 对象的路径](Docs/Assets/README/pipeline.svg)

## 快速开始

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [global.json](global.json) 指定的 .NET SDK 8.0.416。在 `Plugins/AvidScript` 目录运行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine" # 改成你的 UE 5.8 源码目录
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放入一个设为 `Movable` 的 Cube 并选中它。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。菜单使用的源码是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

只编译该样例的 WASM，无需启动 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 代码示例

以下代码摘自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。`UE.Self` 是绑定脚本的 Actor；`deltaSeconds` 是本帧经过的秒数。每次 Tick 按 120 UE 单位/秒更新 X 位置。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 展示了跨帧等待：等待 0.25 秒后修改 Actor。样例还在 `EndPlay` 中取消未完成的等待。

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
```

## 样例

| 场景 | 源码与操作说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 异步加载、Timer、Latent 与取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 当前限制

- 已自动测试 Win64 Editor、打包样例和独立进程网络样例；真实游戏流程、真实多人联机及 Android/iOS 仍需验收。
- 编译器只支持已实现的 C# 和 UE API 子集，不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 支持直接 `await`，也支持在方法开头声明最多 8 个独立变量后跨帧等待；别名、重赋值和其他 `Task<T>` 尚不支持。见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `try/finally` 可用，但其中不能 `await`；`catch` 和 `throw` 尚未接入常规构建入口。见 [异常合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

代码在 [Source/](Source/)（UE 插件）、[Tools/](Tools/)（C# 工具链）和 [Build/](Build/)（构建脚本）。设计与验证记录见 [Docs/](Docs/)，贡献前请阅读 [AGENTS.md](AGENTS.md)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
