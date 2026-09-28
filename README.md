# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white)
![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white)
![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white)
[![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

在 Unreal Engine 中运行 C# 脚本。AvidScript 把 C# 编译为 WebAssembly，由 UE 插件加载和执行；运行游戏时不需要 CLR。当前是开发预览版，主要验证环境为 **UE 5.8 / Win64 Editor**。

![C# 到 UE 对象的编译与调用路径](Docs/Assets/README/pipeline.svg)

## Quick start

需要 UE 5.8 源码版 C++ 项目、Visual Studio 2022（含 UE C++ 工具链）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在 **UE 项目根目录**运行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor target。以下以本仓库所在的 `AvidTPSTemplate` 项目为例；接入其他项目时替换项目名和 `.uproject` 路径。

```powershell
$project = (Resolve-Path '../../AvidTPSTemplate.uproject').Path
& 'C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat' `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

在 Editor 中：

1. 打开 `Tools > AvidScript > Create Project C# Gameplay Workspace`。
2. 编辑 `<项目>/Scripts/AvidScript/GameplayScript.cs`。
3. 选中关卡里 RootComponent 可移动的 Actor，执行 `Tools > AvidScript > Build And Bind Project C# Gameplay Script`。
4. 点击 **Play**。生成的脚本在 `BeginPlay` 设置缩放，并在 `Tick` 旋转 Actor。

修改脚本后，停止 Play，再执行一次 Build And Bind。创建 workspace 不会覆盖已有的 `GameplayScript.cs`。生成文件和编译产物的位置见 [Workspace 说明](Docs/Phase44/P44.3_Project_CSharp_Gameplay_Workspace.md)。

## Example

把生成脚本里的 `Tick` 方法替换为下面的代码，Actor 就会沿 X 轴以 **120 cm/s** 移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向绑定脚本的 Actor。`FVector`、`GetActorLocation` 和 `SetActorLocation` 来自 UE API 绑定生成器。完整脚本可参考 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)；也可以选中 Movable Cube，执行 `Build And Bind C# ActorLifecycle Script` 后点击 Play。

## Samples

| 场景 | 入口 |
| --- | --- |
| Actor 生命周期、输入和碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载与取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| C# 定义 Actor / Component | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| RPC、属性复制和 RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ 类型的 C# 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

## Development

在 `Plugins/AvidScript` 目录构建自带的 C# 示例并运行编译器测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

代码位于 `Source/`（UE 模块）、`Tools/`（C# 编译器）、`Build/`（构建入口）和 `Samples/`（示例）。实现说明见[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)，开发进度见[迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## Current limits

- 只支持已实现的 C# / .NET 子集，不能直接运行任意 NuGet 包。目前 `Task<T>` 只覆盖 `Task<int>`；`catch` 和 `finally` 中不能 `await`。
- 方法体可以热重载；修改脚本定义类型的反射签名后，需要重新编译并重启 Editor。标准 `CancellationToken` 的组合用法还没有进入默认 Build And Bind。
- 网络路径有自动化测试，真实多人游戏尚未验收；Shipping、Android 和 iOS 也尚未验收。

详细支持范围见[语言执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)和[异步能力合同](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
