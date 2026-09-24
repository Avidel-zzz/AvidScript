# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

在 Unreal Engine 中运行 C# 游戏脚本。AvidScript 将受支持的 C# 编译为 WebAssembly，由 UE 插件加载，并通过生成的绑定调用引擎 API；游戏进程不加载 CLR。

![C# 源码到 UE 对象的调用路径](Docs/Assets/README/pipeline.svg)

## Quick start

环境：UE 5.8 源码版、Win64、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK 8.0.416。以下命令在 `Plugins/AvidScript` 目录运行。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine"
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡里放置一个设为 `Movable` 的 Cube 并选中它。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。

只编译样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

Play 后没有变化时，检查 Cube 是否挂有 `AvidScript Component`，并在 Output Log 中搜索 `AvidScript`。

## Script example

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每帧读取并更新 Actor 位置；下面这一段以每秒 120 UE 单位沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向当前绑定的 Actor。完整样例还演示 BeginPlay、异步加载、输入和碰撞回调。

## Samples

| 用途 | 代码与说明 |
| --- | --- |
| Actor 生命周期、变换、输入与碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 下一帧、计时器、异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

脚本声明的 `UClass`、`UProperty` 或 `UFunction` 发生变化后，需重新构建并重启 Editor；参见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## Status

这是开发中的插件，当前以 Win64 为主要验证平台。Editor 与打包样例有自动化测试；真实游戏流程、真实多人联机和 Android/iOS 尚未验收。网络样例已通过独立进程测试。

- 支持的是本项目实现的 C# 语法和 UE API 子集，不能直接运行任意 .NET 项目或 NuGet 包。
- `try` / `catch` / `finally` 的受限写法目前只在[专用测试入口](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)可用。
- `Task<int>` 的直接调用和 `await` 已有[语义合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)，尚不能编译为可执行的 Guest/WASM 脚本。

## Development

运行 C# Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

UE 模块见 [Source/](Source/)，C# 工具链见 [Tools/](Tools/)，构建脚本见 [Build/](Build/)。开发约定见 [AGENTS.md](AGENTS.md)，设计与测试记录见 [Docs/](Docs/)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
