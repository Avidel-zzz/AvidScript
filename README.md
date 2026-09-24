# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

在 Unreal Engine 5.8 中运行 C# 游戏脚本。构建时把支持的 C# 代码编译为 WASM；游戏运行时由 UE 插件加载 WASM，通过生成的绑定调用引擎 API，不加载 CLR。当前主要在 Win64 上开发和验证。

![从 C# 源码到 UE Runtime 的流程](Docs/Assets/README/pipeline.svg)

## Quick start

环境：UE 5.8 源码版、Win64、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、[.NET SDK 8.0.416](global.json)。下面的命令均在 `Plugins/AvidScript` 目录执行。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\Path\To\UnrealEngine"
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放一个 **Movable** 的 Cube 并选中它。执行菜单 `Tools > AvidScript > Build And Bind C# ActorLifecycle Script`，然后点击 Play。Cube 应该沿 X 轴移动，同时旋转、放大。

如果只想编译样例，不启动 Editor 或绑定 Actor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

如果 Play 后没有变化，检查 Cube 上是否添加了 `AvidScript Component`，再到 Output Log 搜索 `AvidScript`。

## Script example

以下是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick`。`UE.Self` 是当前绑定的 Actor，`deltaSeconds` 用于保持每秒 120 UE 单位的移动速度。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

## Samples

| 要做什么 | 从这里开始 |
| --- | --- |
| Actor 生命周期、变换、输入与碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 下一帧、计时器、异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md)、[ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

更改脚本声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor；具体步骤在 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## Current support

| 范围 | 状态 |
| --- | --- |
| Win64 Editor 与打包样例 | 有自动化测试；实际游戏流程仍需单独验收 |
| Android / iOS | 尚未验收 |
| C# | 支持项目实现的语法子集；不能直接运行任意 .NET 项目或 NuGet 包 |
| RPC / 复制属性 / RepNotify | 有独立进程测试；真实多人游戏联机尚未验收 |
| `try` / `catch` / `finally` | 受限写法仅在专用测试入口可用，普通脚本构建尚未启用；见[实现范围](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md) |

## Development

运行 C# 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

UE 模块在 [Source/](Source/)，C# 工具链在 [Tools/](Tools/)，构建脚本在 [Build/](Build/)。开发约定见 [AGENTS.md](AGENTS.md)，设计与测试记录见 [Docs/](Docs/)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
