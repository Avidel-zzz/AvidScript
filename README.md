# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。工具链将支持的 C# 代码编译为 WebAssembly，UE 插件加载 WASM 并通过生成的绑定调用引擎 API；游戏运行时不需要 CLR。

![C# 编译为 WASM 并调用 UE API](Docs/Assets/README/pipeline.svg)

**当前目标平台：** UE 5.8 源码版、Win64。编译器仅支持已实现的 C# 子集，不能直接运行任意 .NET 项目或 NuGet 包。

## Quick start

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [global.json](global.json) 指定的 .NET SDK。以下命令在 `Plugins/AvidScript` 目录执行。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\Path\To\UnrealEngine"
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开工程，在关卡中放置并选中一个 **Movable** Cube，然后运行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**。点击 **Play** 后，Cube 会移动、旋转并放大。只生成样例 WASM 可运行：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

如果 Cube 没有变化，检查其 **AvidScript Component**，并在 **Output Log** 中搜索 `AvidScript`。

## 代码示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 每秒让 Actor 沿 X 轴移动 120 个 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

更多可运行样例：

| 功能 | 样例 |
| --- | --- |
| Actor 生命周期与变换 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 下一帧、计时器、异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

新增或修改 C# 定义的 `UClass`、`UProperty`、`UFunction` 后，需要重新构建并重启 Editor；完整代码和步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 支持范围

| 项目 | 当前状态 |
| --- | --- |
| Win64 Editor 与打包样例 | 已有自动化验证 |
| RPC、属性复制、RepNotify | 已有独立进程测试；实际游戏多人联机仍待验收 |
| Android、iOS | 尚未验收 |
| `try` / `catch` / `finally` | 专用测试入口支持受限的抛出、捕获和嵌套清理；普通脚本构建尚未启用，见[实现范围](Docs/Phase66/P66.C_Language_Execution_Plan.md) |

UE 模块在 [`Source/`](Source/)，C# 编译器在 [`Tools/`](Tools/)，构建入口在 [`Build/`](Build/)。设计与验证记录见 [`Docs/`](Docs/)，仓库开发约定见 [AGENTS.md](AGENTS.md)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
