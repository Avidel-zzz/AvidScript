# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-172A34?logo=unrealengine&logoColor=white) ![C#](https://img.shields.io/badge/Language-C%23-512BD4?logo=dotnet&logoColor=white) ![WebAssembly](https://img.shields.io/badge/Target-WebAssembly-5541A9?logo=webassembly&logoColor=white) ![Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-0967A6?logo=windows&logoColor=white) ![Preview](https://img.shields.io/badge/Status-Preview-805413) [![MIT](https://img.shields.io/badge/License-MIT-226342)](LICENSE)

C# 脚本编译为 WebAssembly，在 Unreal Engine 中运行。编译器使用 Roslyn；UE 运行时通过生成的绑定调用引擎 API，不加载 .NET/CLR。

当前支持 **UE 5.8 源码版 + Windows x64**。项目仍处于 Preview；C# 和 UE API 的限制见[支持范围](#支持范围)。不能直接运行任意 .NET 程序或 NuGet 包。

<img src="Docs/Assets/README/avidscript-hero.png" width="800" alt="AvidScript C# gameplay preview">

## 快速开始

准备 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git 和 [.NET SDK 8.0.416](global.json)。将仓库放到 C++ UE 项目的 `Plugins/AvidScript`，以下命令均在该目录执行。

1. 安装 Wasmtime 依赖：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建 Editor target；替换引擎路径、项目路径和 target 名：

   ```powershell
   $ueRoot = "C:\Path\To\UnrealEngine"
   $project = "C:\Path\To\YourProject.uproject"
   & "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
     YourProjectEditor Win64 Development "-Project=$project" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 打开 Editor，在关卡中放置并选中一个 **Movable** Cube。
4. 运行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，再点击 **Play**。Cube 会移动、旋转并放大。

菜单会编译 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 并绑定到选中的 Actor。若无变化，检查 Actor 的 **AvidScript Component** 和 **Output Log** 中的 `AvidScript` 错误。只编译脚本可运行：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 代码示例

**Actor Tick。** 以下方法来自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)，每帧更新位置、旋转和缩放：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    ElapsedSeconds += deltaSeconds;
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
    FRotator currentRotation = UE.Self.GetActorRotation();
    UE.Self.SetActorRotation(currentRotation + new FRotator(0.0f, 90.0f * deltaSeconds, 0.0f));
    FVector currentScale = UE.Self.GetActorScale3D();
    UE.Self.SetActorScale3D(currentScale + new FVector(0.0f, 0.0f, 0.6f * deltaSeconds));
}
```

`UE.Self` 指向绑定的 Actor；`deltaSeconds` 是本帧秒数。此例沿 X 轴每秒移动 120 个 UE 单位。

**C# 定义 UE 类型。** [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 中的 `Projectile` 包含以下成员（节选）：

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

`UClass`、`UProperty` 和 `UFunction` 会生成对应的 UE 声明。修改这些声明后需要重建并重启 Editor；`NextTickAsync()` 在下一帧恢复后才写入属性。

## 支持范围

| 功能 | 当前情况 | 示例 / 限制 |
| --- | --- | --- |
| C# 语言 | 控制流、数组、部分类/结构体、lambda、局部函数、受限泛型 | [泛型示例](Fixtures/Phase66/GenericMethods.cs)；只编译明确支持的 C# 子集 |
| `foreach` | 数组及受限 sealed class 枚举器；同步退出时调用 `Dispose` | [清理示例](Fixtures/Phase66/EnumeratorCleanup.cs)；其他枚举器形态和异常退出清理未支持 |
| `try/finally` | 同步正常结束、`return`、`break`、`continue` 的清理 | [示例](Fixtures/Phase66/FinallyCleanup.cs)；`catch`、`throw`、跨 `await` 清理未支持 |
| 异步 | 下一帧、计时器、资源加载及部分 UE Latent API | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md)；不支持任意 `Task` 或自定义 awaiter |
| UE 集成 | 生成的 API 绑定、脚本定义 UE 类型、RPC/复制属性/RepNotify 聚焦测试 | 真实客户端/服务器玩法仍待验收 |
| 平台与工具 | Windows Editor 和打包样例、部分调试与热重载工具 | Android 真机和 iOS 尚未验收 |

完整语言限制见 [P66.C 执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。[P66 实施计划](Docs/Phase66/P66.1_Implementation_Plan.md)记录当前里程碑；[性能报告](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md)提供测试配置和数据，不代表整体性能领先 Puerts 或 Unreal AngelScript。

## 示例

| 场景 | 入口 |
| --- | --- |
| Actor 生命周期、Tick | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 拾取、延迟恢复 | [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| RPC、复制属性、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 计时器、异步加载、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |

## 项目结构

![C# 编译与 UE 执行路径](Docs/Assets/README/script-to-game.png)

`C# → Roslyn → Guest IR → WASM → AvidScript Runtime (Wasmtime/WAMR) → UE`。UE 对象经受验证的 `ObjectHandle` 访问，不向脚本暴露裸 `UObject*`。

| 目录 | 内容 |
| --- | --- |
| [`Source/`](Source/) | UE Runtime、Editor、VM 后端和绑定模块 |
| [`Tools/`](Tools/) | C# 前端、Guest IR 与 WASM 编译工具 |
| [`Build/`](Build/) | 依赖安装、构建与验证脚本 |
| [`Samples/`](Samples/) | 游戏脚本示例 |
| [`Docs/`](Docs/) | 设计、验证证据和阶段计划 |

开发文档：[IDE 工作区](Docs/Phase61/P61.D4c2_Editor_IDE_Commands.md) · [调试面板](Docs/Phase61/P61.C4b_Editor_Debugger_Panel.md) · [仓库工作规则](AGENTS.md)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
