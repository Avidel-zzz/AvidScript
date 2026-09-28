# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white)
![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white)
![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white)
[![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。构建时把 C# 编译成 WebAssembly，UE 插件负责加载脚本和调用引擎 API；游戏进程不需要 CLR。目前是开发预览版，主要在 **UE 5.8 / Win64 Editor** 上验证。

## 先看代码

下面的方法可以替换项目生成的 `Scripts/AvidScript/GameplayScript.cs` 中的 `Tick`。绑定到 Actor 后，它会以每秒 90 度的速度改变 yaw：

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

`UE.Self` 是当前脚本绑定的 Actor；`FRotator` 和 Actor 方法由 UE 绑定生成器提供。生成的文件已包含这个方法所需的 `using` 和类定义。完整示例见 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

## 安装与运行

需要 UE 5.8 源码版 C++ 项目、Visual Studio 2022 的 UE C++ 工具链、PowerShell 7，以及 [.NET SDK 8.0.416](global.json)。在 **UE 项目根目录**执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译项目的 Editor target。将 `YourGame` 换成自己的项目名，并按本机位置修改 `$ueRoot`：

```powershell
$projectName = 'YourGame'
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path "../../$projectName.uproject").Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  "${projectName}Editor" Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开项目，在 Editor 的 `Tools > AvidScript` 菜单中：

1. 执行 `Create Project C# Gameplay Workspace`，生成 `Scripts/AvidScript/GameplayScript.cs`。
2. 编辑脚本，选中关卡中 RootComponent 可移动的 Actor。
3. 执行 `Build And Bind Project C# Gameplay Script`，然后点击 **Play**。

默认脚本会在 `BeginPlay` 设置缩放，并在 `Tick` 改变旋转。修改脚本后，停止 Play，再执行一次 Build And Bind。创建 workspace 不会覆盖已编辑的脚本；生成目录和产物位置见 [Workspace 说明](Docs/Phase44/P44.3_Project_CSharp_Gameplay_Workspace.md)。

## 构建路径

![C# 经 Roslyn 和 Guest IR 编译为 WASM，再由 UE 插件调用引擎对象](Docs/Assets/README/pipeline.svg)

源码经过 Roslyn、Guest IR 和 WASM backend；运行时通过生成的绑定访问 UE 对象。实现分层见 [模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)。

## 示例

| 内容 | 示例 |
| --- | --- |
| Actor 生命周期、输入和碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载和取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| C# 定义 Actor / Component | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| RPC、属性复制和 RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 和存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API 的 C# 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

## 从源码验证

在 `Plugins/AvidScript` 目录运行示例构建和 C# 编译器自执行测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

`Source/` 是 UE 模块，`Tools/` 是编译器和生成工具，`Build/` 是构建入口，`Samples/` 是示例。当前开发项见 [迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## 当前限制

- 支持的是已实现的 C# / .NET 子集，不能直接运行任意 NuGet 包。`Task<T>` 目前只覆盖 `Task<int>`，`catch` 和 `finally` 中不能 `await`。
- 方法体热重载已有自动化覆盖；脚本定义类型的反射签名变化仍需重新编译并重启 Editor。标准 `CancellationToken` 的组合用法尚未进入默认 Build And Bind。
- 网络路径有自动化测试，真实多人游戏尚未验收；Shipping、Android 和 iOS 也尚未验收。

语言支持范围见 [P66.C 执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)和[异步能力合同](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
