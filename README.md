# AvidScript

![AvidScript：C# 脚本驱动 Unreal Engine Actor](Docs/Assets/README/avidscript-hero.png)

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-172A34?logo=unrealengine&logoColor=white) ![C#](https://img.shields.io/badge/Language-C%23-512BD4?logo=dotnet&logoColor=white) ![WebAssembly](https://img.shields.io/badge/Target-WebAssembly-5541A9?logo=webassembly&logoColor=white) ![Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-0967A6?logo=windows&logoColor=white) ![Preview](https://img.shields.io/badge/Status-0.1.0%20Preview-805413) [![MIT License](https://img.shields.io/badge/License-MIT-226342)](LICENSE)

AvidScript 把受支持的 C# 游戏脚本编译为 WebAssembly，在 Unreal Engine 中运行。UE Runtime 不加载 .NET/CLR。

**当前环境：** UE 5.8 源码版、Windows x64。项目仍处于 Preview；支持的是明确列出的 C# 与 UE API 子集，不能直接运行任意 .NET 程序或 NuGet 包。

## 快速开始

要求：Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git、[.NET SDK 8.0.416](global.json)。以下命令在项目的 `Plugins/AvidScript` 目录执行。

1. 将仓库放在 C++ UE 项目的 `Plugins/AvidScript`，安装 Wasmtime 依赖：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建 Editor；替换引擎路径、项目路径和 target 名：

   ```powershell
   $ueRoot = "C:\Path\To\UnrealEngine"
   $project = "C:\Path\To\YourProject.uproject"
   & "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
     YourProjectEditor Win64 Development "-Project=$project" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 打开 Editor，在关卡中放一个 Cube，将 **Mobility** 设为 **Movable** 并选中它。
4. 运行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。

该菜单会编译 [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 并将脚本绑定到选中的 Actor。若没有变化，检查 Actor 上的 **AvidScript Component** 和 **Output Log** 中的 `AvidScript` 错误。只编译脚本可运行 `pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1`；此命令不会修改关卡。

## 代码示例

每帧移动 Actor，摘自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向绑定脚本的 Actor；`deltaSeconds` 是本帧时长。上面的位移速度为每秒 120 个 UE 单位。

等待下一帧再修改属性，摘自 [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)：

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

这里的 `UClass`、`UProperty` 和 `UFunction` 对应生成的 UE 类、属性和函数。修改这些反射声明后，需要重建 Editor 并重启；受支持的方法体修改可热重载。计时器、异步资源加载和对象销毁时取消的样例见 [LatentGameplay](Samples/CSharp/LatentGameplay/README.md)。

![异步等待、恢复和取消](Docs/Assets/README/async-lifecycle.png)

## 工作原理

![C# 源码到 UE 游戏对象](Docs/Assets/README/script-to-game.png)

构建时，Roslyn 前端将受支持的 C# 降低为 Guest IR，再生成 WASM。项目选定 UE API 后，绑定生成器提供 C# 接口和 UE 描述符。运行时通过受验证的对象句柄调用 UE；主要 VM 后端为 Wasmtime，另有 WAMR 后端。

## 样例

| 想做什么 | 从这里开始 |
| --- | --- |
| Actor 生命周期、Tick | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 道具拾取、延迟恢复 | [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| 为项目 C++ API 生成绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

## 支持状态

| 范围 | 当前状态 |
| --- | --- |
| C# | 常用控制流、数组、受支持的类/结构体、lambda、局部函数、封闭泛型方法与简单泛型类的同步成员；无 `catch` 的同步 `try/finally` 支持正常结束、`return`、`break` 和 `continue`；[泛型样例](Fixtures/Phase66/GenericMethods.cs) · [清理样例](Fixtures/Phase66/FinallyCleanup.cs) |
| 异步 | 下一帧、计时器、资源加载、部分 UE Latent API；不支持任意 `Task` 或自定义 awaiter |
| UE | 生成的 API 绑定、脚本定义的 UE 类型，以及 RPC/复制属性/RepNotify 的聚焦测试；真实网络玩法仍待验收 |
| 工具与平台 | 错误定位、部分调试与热重载、Windows 打包样例；Android 真机和 iOS 尚未验收 |

完整进度见 [P66 实施计划](Docs/Phase66/P66.1_Implementation_Plan.md)。性能数据和测试配置见[性能报告](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md)；现有结果不能代表整体领先 Puerts 或 Unreal AngelScript。

同步 `try/finally` 尚不支持 `catch`、`throw`、跨 `await` 清理或普通枚举器的 `Dispose`；边界和可复现测试见 [P66.C 执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

## 仓库结构与文档

| 目录 | 内容 |
| --- | --- |
| [`Source/`](Source/) | UE Runtime、Editor、VM 后端与绑定模块 |
| [`Tools/`](Tools/) | C# 前端、Guest 编译器和生成工具 |
| [`Build/`](Build/) | 依赖、构建与验证脚本 |
| [`Samples/`](Samples/) | 游戏脚本样例 |
| [`Docs/`](Docs/) | 设计、测试证据与阶段计划 |

[IDE 工作区](Docs/Phase61/P61.D4c2_Editor_IDE_Commands.md) · [调试面板](Docs/Phase61/P61.C4b_Editor_Debugger_Panel.md) · [Windows 打包样例](Docs/Phase64/P64.D_Packaged_UI.md) · [插件安装](Docs/Phase65/P65.A_Deterministic_Release_And_Atomic_Install.md) · [仓库工作规则](AGENTS.md)

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
