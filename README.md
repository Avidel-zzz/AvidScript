# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white)
![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white)
![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white)
[![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。构建时将 C# 编译为 WebAssembly；运行时由 UE 插件加载 `.wasm`，调用 Actor 等引擎对象，游戏进程不需要 CLR。

**Status:** 开发预览版，主要验证环境为 UE 5.8 源码版和 Win64 Editor。Windows Shipping、Android、iOS 及真实多人游戏仍待验收。

![C# 源码编译为 WASM，并通过 UE 插件调用引擎对象](Docs/Assets/README/pipeline.svg)

## Example

把项目生成的 `Scripts/AvidScript/GameplayScript.cs` 中的 `Tick` 方法替换为以下代码。运行后，绑定的 Actor 每秒绕 yaw 旋转 90 度：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FRotator rotation = UE.Self.GetActorRotation();
    UE.Self.SetActorRotation(new FRotator(
        rotation.Pitch,
        rotation.Yaw + 90.0f * deltaSeconds,
        rotation.Roll), false);
}
```

`UE.Self` 是当前绑定的 Actor；`FRotator`、`GetActorRotation` 和 `SetActorRotation` 来自生成的 UE API 绑定。生成文件已有所需的类和 `using`。更多调用见 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

## Quick start

需要 UE 5.8 源码版 C++ 项目、Visual Studio 2022 UE C++ 工具链、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在 **UE 项目根目录**执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor target。将 `YourGame` 换成项目名，并修改 UE 源码路径：

```powershell
$projectName = 'YourGame'
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path "../../$projectName.uproject").Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  "${projectName}Editor" Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开项目，在 Editor 的 `Tools > AvidScript` 菜单中：

1. 运行 `Create Project C# Gameplay Workspace`，生成 `<项目>/Scripts/AvidScript/GameplayScript.cs`。
2. 在关卡中选中 RootComponent 可移动的 Actor。
3. 运行 `Build And Bind Project C# Gameplay Script`，然后点击 Play。

默认脚本在 `BeginPlay` 设置缩放，在 `Tick` 旋转 Actor。修改脚本后，停止 Play，再运行一次 Build And Bind。Workspace 创建命令不会覆盖已有脚本；文件位置见 [Project C# Workspace](Docs/Phase44/P44.3_Project_CSharp_Gameplay_Workspace.md)。

## Samples

| 场景 | 源码 / 说明 |
| --- | --- |
| Actor 生命周期、输入和碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载和取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| C# 定义 Actor / Component | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| RPC、属性复制和 RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 和存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API 的 C# 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

## Development

在 `Plugins/AvidScript` 目录构建示例并运行编译器自执行测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

| 目录 | 内容 |
| --- | --- |
| [`Source/`](Source/) | UE Runtime、Editor 模块与绑定 |
| [`Tools/`](Tools/) | C# 编译器与生成工具 |
| [`Build/`](Build/) | 构建和验证入口 |
| [`Samples/`](Samples/) | 脚本与项目示例 |

设计与进度：[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md) · [迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## Limitations

- 支持的是已实现的 C# / .NET 子集，不能直接运行任意 NuGet 包。`Task<T>` 目前只覆盖 `Task<int>`，`catch` 和 `finally` 中不能 `await`。
- 方法体热重载已有自动化覆盖；脚本定义类型的反射签名变化仍需重新编译并重启 Editor。标准 `CancellationToken` 的组合用法尚未进入默认 Build And Bind。
- 网络路径有自动化测试，真实多人游戏尚未验收；Shipping、Android 和 iOS 也尚未验收。

语言边界见 [P66.C 执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)和[异步能力合同](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
