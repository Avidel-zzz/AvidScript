# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

在 Unreal Engine 5.8 中运行 C# 游戏脚本。工具链把受支持的 C# 代码编译为 WASM，UE 插件加载模块并通过生成的绑定调用引擎 API。目前以 Win64 为主要开发和验证平台。

![C# 源码经 Roslyn、Guest IR 和 WASM 调用 UE 对象](Docs/Assets/README/pipeline.svg)

## 快速开始

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK。以下命令从 `Plugins/AvidScript` 执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 改成你的 UE 5.8 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡里放一个设为 **Movable** 的 Cube 并选中它。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，再进入 **Play**。Cube 会移动、旋转并放大。

如果只想编译这个样例的 WASM，运行 `pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1`。

## 脚本示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 每秒让 Actor 沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 展示 `await` 和生命周期取消。`BeginPlay` 等待 0.25 秒后修改缩放；等待期间若发生 `EndPlay`，后续代码不会执行：

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

![Delay 完成与 EndPlay 取消的执行路径](Docs/Assets/README/async-lifecycle.svg)

## 样例

| 场景 | 源码 / 说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 从 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 已知限制

- 支持的是 C# 和 UE API 子集，不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 只支持受限的同源静态调用与 `await`；跨 Session 等待尚未支持。见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `catch/throw` 需指定 `-LanguageErrors bounded`，生成类型中的这一模式目前限 Win64 Development；异步方法内抛错、`await` 后捕获以及 `finally` 中的 `await` 尚未支持。见 [语言错误合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改脚本定义的 `UClass`、`UProperty` 或 `UFunction` 后，需重新构建并重启 Editor。
- Android、iOS、真实多人联机和完整游戏流程仍待验收。

## 开发

运行 C# Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

目录入口：[Source/](Source/)（UE 插件）、[Tools/](Tools/)（C# 工具链）、[Build/](Build/)（构建脚本）、[Docs/](Docs/)（设计与验证记录）。仓库开发规则见 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
