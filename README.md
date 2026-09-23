# AvidScript

![AvidScript](Docs/Assets/README/avidscript-hero.png)

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-172A34?logo=unrealengine&logoColor=white) ![C#](https://img.shields.io/badge/Language-C%23-512BD4?logo=dotnet&logoColor=white) ![WebAssembly](https://img.shields.io/badge/Target-WebAssembly-5541A9?logo=webassembly&logoColor=white) ![Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-0967A6?logo=windows&logoColor=white) ![Preview](https://img.shields.io/badge/Status-Preview-805413) [![MIT](https://img.shields.io/badge/License-MIT-226342)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。构建工具把受支持的 C# 编译为 WebAssembly；UE 运行时加载 WASM，通过生成的绑定调用引擎 API，不加载 .NET/CLR。

**当前可用环境：** UE 5.8 源码版、Windows x64。项目仍在开发中，支持范围见下文；它不能直接运行任意 .NET 程序或 NuGet 包。

## 看一段脚本

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `avid_on_tick` 入口每帧移动 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor；`deltaSeconds` 是本帧的秒数。上例沿 X 轴以每秒 120 个 UE 单位移动。

也可以在 C# 中声明 UE 类型。[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 中的 `Projectile` 暴露了属性和可从 Blueprint 调用的方法：

```csharp
[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public async void SetLaunchSpeedNextTick(float speed)
    {
        await AvidContinuations.NextTickAsync();
        LaunchSpeed = speed;
    }
}
```

第二段摘取了原文件中的相关成员。`NextTickAsync()` 恢复后才写入属性；修改 `UClass`/`UProperty`/`UFunction` 声明需要重建并重启 Editor。

## 运行示例

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git 和 [.NET SDK 8.0.416](global.json)。将本仓库放到 C++ UE 项目的 `Plugins/AvidScript`，在该目录执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建项目的 Editor target（替换三个占位路径/名称）：

```powershell
$ueRoot = "C:\Path\To\UnrealEngine"
$project = "C:\Path\To\YourProject.uproject"
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 Editor，在关卡中放置并选中一个 Cube，把 **Mobility** 设为 **Movable**。运行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，再点击 **Play**；Cube 应移动、旋转并放大。没有变化时，检查 Actor 上的 **AvidScript Component** 和 **Output Log** 中的 `AvidScript` 错误。

只编译脚本、不修改关卡：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 示例索引

| 场景 | 代码 |
| --- | --- |
| Actor 生命周期、Tick | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 拾取、延迟恢复 | [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| RPC、复制属性、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) / [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 计时器、异步加载、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |

## 实现与边界

![C# 到 UE 的执行路径](Docs/Assets/README/script-to-game.png)

Roslyn 前端生成版本化 Guest IR，再编译为 WASM。UE 运行时通过受验证的对象句柄访问 UObject；VM 后端包括 Wasmtime 和 WAMR。

| 已有实现 | 当前边界 |
| --- | --- |
| 控制流、数组、部分 C# 类/结构体、lambda、局部函数和受限泛型 | 只支持编译器明确接受的 C# 子集；示例见 [GenericMethods](Fixtures/Phase66/GenericMethods.cs) |
| 数组 `foreach`、受限 sealed class 枚举器的同步 `foreach` 与自动 `Dispose` | 其他枚举器形态和异常退出清理尚未支持；[可运行示例](Fixtures/Phase66/EnumeratorCleanup.cs) |
| 同步 `try/finally` 的正常结束、`return`、`break`、`continue` 清理 | 尚不支持 `catch`、`throw`、跨 `await` 的清理；[清理示例](Fixtures/Phase66/FinallyCleanup.cs) |
| 下一帧、计时器、资源加载和部分 UE Latent API | 不支持任意 `Task` 或自定义 awaiter |
| 脚本定义的 UE 类型、生成的 API 绑定，以及 RPC/复制属性/RepNotify 聚焦测试 | 真实客户端/服务器玩法仍需验收 |
| Windows Editor 和打包样例、部分调试与热重载工具 | Android 真机和 iOS 尚未验收 |

当前里程碑和剩余工作见 [P66 实施计划](Docs/Phase66/P66.1_Implementation_Plan.md)；具体语言限制见 [P66.C 执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。[性能报告](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md)列出测试配置与数据，不能据此推断整体性能领先 Puerts 或 Unreal AngelScript。

代码目录：[`Source/`](Source/)（UE 插件）、[`Tools/`](Tools/)（C# 编译工具）、[`Build/`](Build/)（构建与验证）、[`Samples/`](Samples/)（示例）。开发入口见 [IDE 工作区](Docs/Phase61/P61.D4c2_Editor_IDE_Commands.md)、[调试面板](Docs/Phase61/P61.C4b_Editor_Debugger_Panel.md)和[仓库工作规则](AGENTS.md)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
