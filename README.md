# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 5.8 的 C# 脚本插件。构建时将 C# 编译为 WebAssembly；运行时由 UE 插件加载 WASM、调用引擎 API，无需在游戏中加载 CLR。

![AvidScript 编译与运行流程](Docs/Assets/README/pipeline.svg)

## Quick start

需要 UE 5.8 源码版、Win64、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [global.json](global.json) 指定的 .NET SDK 8.0.416。以下命令在 `Plugins/AvidScript` 目录执行。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\Path\To\UnrealEngine"
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable Cube**，运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。

只生成样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

如果 Play 后 Cube 没有变化，检查它是否挂有 **AvidScript Component**，并在 **Output Log** 中搜索 `AvidScript`。

## Example

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 每帧读取当前位置，并让 Actor 沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向当前绑定的 Actor。完整样例还包含 BeginPlay、异步加载、输入和碰撞回调。

## Samples

| 场景 | 代码与说明 |
| --- | --- |
| Actor 生命周期与变换 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 下一帧、计时器、异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 绑定项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

修改脚本声明的 `UClass`、`UProperty` 或 `UFunction` 后，需重新构建并重启 Editor；步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## Status

- **Win64：** Editor 与打包样例有自动化测试。Android 和 iOS 尚未验收。
- **C#：** 支持项目实现的语法子集，不能直接运行任意 .NET 项目或 NuGet 包。
- **网络：** RPC、属性复制和 RepNotify 已通过独立进程测试；实际游戏多人联机仍待验收。
- **异常：** 专用测试入口可执行受限的 `try { throw new System.Exception(); } finally { Count = Count + 1; }`，包括嵌套清理；普通脚本构建尚未启用。见[当前实现范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

## Development

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

UE 模块在 [Source/](Source/)，C# 编译器在 [Tools/](Tools/)，构建入口在 [Build/](Build/)。开发约定见 [AGENTS.md](AGENTS.md)，设计与测试记录见 [Docs/](Docs/)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
