# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

UE 5.8 的 C# → WASM 脚本插件。C# 在构建时编译为 WASM；游戏运行时加载 WASM，通过生成的绑定调用 UE API，不加载 CLR。

![C# → WASM → UE Runtime](Docs/Assets/README/pipeline.svg)

## 运行样例

需要 **UE 5.8 源码版、Win64、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7** 和 [global.json](global.json) 指定的 **.NET SDK 8.0.416**。以下命令在 `Plugins/AvidScript` 目录执行，使用本仓库的 `AvidTPSTemplate.uproject`。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\Path\To\UnrealEngine"
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开工程，在关卡中放置并选中一个 **Movable Cube**。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 应移动、旋转并放大。

只编译这个样例、不在 Editor 中绑定 Actor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

运行后没有变化时，检查 Cube 上的 **AvidScript Component**，并在 **Output Log** 中搜索 `AvidScript`。

## 脚本代码

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

Actor 沿 X 轴以每秒 120 个 UE 单位移动。`UE.Self` 是样例的当前 Actor 绑定，完整代码和类型定义在上面的源文件中。

## 样例目录

| 要实现的功能 | 从这里开始 |
| --- | --- |
| Actor 生命周期与变换 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 下一帧、计时器、异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

修改脚本声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor；参见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 当前限制

- 主要验证平台是 **Win64**。Editor 和打包样例有自动化测试；Android、iOS 尚未验收。
- C# 编译器支持已实现的语法子集，不能直接运行任意 .NET 项目或 NuGet 包。
- RPC、属性复制和 RepNotify 已通过独立进程测试；实际游戏多人联机仍待验收。
- `try` / `catch` / `finally` 目前仅在专用测试入口支持部分写法，普通脚本构建尚未启用。详见 [P66.C 实现范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

代码在 [Source/](Source/)（UE 模块）、[Tools/](Tools/)（C# 编译器）和 [Build/](Build/)（构建脚本）。设计与测试记录在 [Docs/](Docs/)；开发约定见 [AGENTS.md](AGENTS.md)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
