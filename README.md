# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 UE 5.8 的 C# 脚本插件。脚本编译为 WASM，在 UE 进程中运行；引擎 API 通过生成的绑定调用，运行时不加载 CLR。

![Script.cs → Roslyn/Guest IR → WASM → UE Runtime → UObject](Docs/Assets/README/pipeline.svg)

## Example

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每帧把 Actor 沿 X 轴移动 120 UE 单位/秒：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`avid_on_tick` 是 UE 调用的入口，`UE.Self` 是绑定的 Actor。异步入口可以等待下一帧后继续执行：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    await AvidContinuations.NextTickAsync();
    UE.Self.AddActorWorldOffset(new FVector(0.0f, 0.0f, 5.0f));
}
```

完整样例还包含异步加载、输入和碰撞回调。

## Build and run

需要 Win64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [global.json](global.json) 指定的 .NET SDK 8.0.416。在 `Plugins/AvidScript` 目录运行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine"
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放一个 `Movable` Cube 并选中它。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，再点击 **Play**。预期结果：Cube 移动、旋转并放大。如果没有变化，检查 Cube 上的 `AvidScript Component`，并在 Output Log 搜索 `AvidScript`。

只编译样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Samples

| 用途 | 代码与说明 |
| --- | --- |
| Actor 生命周期、变换、输入与碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 下一帧、计时器、异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

脚本声明的 `UClass`、`UProperty` 或 `UFunction` 发生变化后，需重新构建并重启 Editor；参见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## Current support

当前验证集中在 Win64。Editor、打包样例和网络样例分别有自动化测试，网络样例使用独立进程；真实游戏流程、真实多人联机以及 Android/iOS 尚未验收。

已知限制：

- 编译器只支持本项目实现的 C# 语法和 UE API 子集，不能直接运行任意 .NET 项目或 NuGet 包。
- `try` / `catch` / `finally` 的受限写法目前只在[专用测试入口](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)可用。
- `int score = await LoadScoreAsync();` 这样的 `Task<int>` 调用目前还不能从 C# 编译为可执行 WASM；进度见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。

## Development

运行 C# Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

[Source/](Source/) 是 UE 插件，[Tools/](Tools/) 是 C# 编译器和相关工具，[Build/](Build/) 是构建入口。开发约定见 [AGENTS.md](AGENTS.md)，设计与验证记录见 [Docs/](Docs/)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
