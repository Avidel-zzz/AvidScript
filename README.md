# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# → WASM 脚本插件。编译器处理支持的 C# 代码，插件通过生成的绑定调用 UE API。目前主要在 Win64 上开发和验证。

![C# 源码、WASM 模块与 UE Runtime 的调用路径](Docs/Assets/README/pipeline.svg)

## 代码示例

以下代码来自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。Actor 每秒沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 在 `BeginPlay` 等待 0.25 秒后修改缩放；`EndPlay` 会取消尚未完成的等待：

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
```

![Delay 完成与 EndPlay 取消的路径](Docs/Assets/README/async-lifecycle.svg)

## 运行 ActorLifecycle 样例

需要 UE 5.8 源码版、Visual Studio 2022 的 UE C++ 工作负载、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK。在 `Plugins/AvidScript` 目录执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 改为本机 UE 5.8 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡里放置一个 **Movable** Cube 并选中它。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后进入 **Play**。Cube 会移动、旋转和放大。

只需构建该样例的 WASM 时，可运行 `pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1`。

## 更多样例

| 要做的事 | 样例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 限制

- 编译器支持 C# 与生成的 UE API 子集；不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 支持受限的同源静态调用与 `await`，尚不能跨 Session 等待。详见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `catch/throw` 需使用 `-LanguageErrors bounded`。异步方法中抛错及 `await` 后捕获尚未支持；`finally` 中不能 `await`。生成类型中的该模式目前限 Win64 Development。详见 [语言错误合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改脚本定义的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。
- Android、iOS、真实多人联机及完整游戏流程尚未验收。

## 开发

运行 C# 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

[Source/](Source/) 是 UE 插件，[Tools/](Tools/) 是 C# 工具链，[Build/](Build/) 是构建入口。[Docs/](Docs/) 包含设计与验证记录；仓库工作规则见 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
