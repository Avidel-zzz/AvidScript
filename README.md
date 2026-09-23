# AvidScript

![AvidScript：C# 脚本驱动 Unreal Engine Actor](Docs/Assets/README/avidscript-hero.png)

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-172A34?logo=unrealengine&logoColor=white) ![C#](https://img.shields.io/badge/Language-C%23-512BD4?logo=dotnet&logoColor=white) ![WebAssembly](https://img.shields.io/badge/Target-WebAssembly-5541A9?logo=webassembly&logoColor=white) ![Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-0967A6?logo=windows&logoColor=white) ![Preview](https://img.shields.io/badge/Status-0.1.0%20Preview-805413) [![MIT License](https://img.shields.io/badge/License-MIT-226342)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。构建工具把受支持的 C# 代码编译成 WebAssembly；UE 插件加载脚本并提供 UE API 绑定。游戏运行时不加载 .NET/CLR。

> [!NOTE]
> 当前是 **0.1.0 Preview**，主要验证环境为 **UE 5.8 源码版 + Windows x64**。支持范围见下文；它尚不能直接运行任意 .NET 程序或 NuGet 包。

## 代码示例

下面的 `Tick` 来自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。把脚本绑定到 Actor 后，Actor 会沿 X 轴以每秒 120 个 UE 单位移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是脚本绑定的 Actor；`deltaSeconds` 是这一帧经过的时间。完整样例还包含 `BeginPlay`、旋转、缩放、资源加载和 `EndPlay`。

## 快速开始

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022 的 UE C++ 构建环境、PowerShell 7、Git 和 .NET SDK **8.0.416**。以下命令在项目的 `Plugins/AvidScript` 目录运行。

1. 将仓库放入 C++ UE 项目的 `Plugins/AvidScript`，安装 Wasmtime 依赖：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建项目 Editor。将 target 和项目路径换成自己的：

   ```powershell
   $env:UE_ROOT = "C:\UnrealEngine"

   & "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" `
     YourProjectEditor Win64 Development `
     "-Project=C:\Path\To\YourProject.uproject" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 打开 Editor，在关卡中放一个 Cube，将 **Mobility** 设为 **Movable** 并选中它。
4. 执行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。方块会移动、旋转并放大。

菜单会构建样例并把脚本绑定到选中 Actor 的 **AvidScript Component**。没有看到变化时，检查组件绑定和 **Output Log** 中的 `AvidScript` 错误。需要命令行构建时运行 `pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1`；它只生成脚本，不会修改关卡。

## 其他写法

等待 0.25 秒后继续执行：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    await AvidContinuations.DelayAsync(0.25f);
    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}
```

等待不会阻塞游戏帧；对象或 World 销毁时，关联的等待会取消。下一帧、资源加载及主动取消的写法见 [LatentGameplay](Samples/CSharp/LatentGameplay/README.md)。

![等待、恢复与对象销毁时取消](Docs/Assets/README/async-lifecycle.png)

用 C# 定义可被蓝图调用的 Actor 方法（摘自 [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)）：

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

`UClass`、`UProperty`、`UFunction` 生成 UE 可识别的类、属性和函数。修改这些反射声明后要重建 Editor 并重启；只修改受支持的方法体可使用热重载。[构建和使用说明](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 工作原理

![C# 源码经编译、加载到驱动 UE 游戏对象的流程](Docs/Assets/README/script-to-game.png)

项目先选择要暴露的 UE API，绑定生成器再提供相应的 C# 接口。脚本通过经验证的对象句柄访问 UE 对象，不持有裸 `UObject*`。Wasmtime 是主要执行后端，另有 WAMR 兼容后端。

## 当前范围

| 方面 | 已有支持 | 仍有限制 |
| --- | --- | --- |
| C# | 常用控制流、数组、受支持的类/结构体、lambda、局部函数、封闭泛型方法和[简单泛型类](Fixtures/Phase66/GenericMethods.cs)的同步成员 | 复杂泛型类、异步泛型及任意 .NET/NuGet 库尚不支持 |
| 异步 | 计时器、下一帧、资源加载、受支持的 UE Latent API | 不能等待任意 `Task` 或自定义 awaiter |
| UE 集成 | 生成的 API 绑定、脚本定义的 Actor/Component/Subsystem、RPC、属性复制与 RepNotify 的聚焦测试 | 反射结构变更需重建 Editor；真实网络玩法仍需单独验收 |
| 工具与平台 | 错误定位、受支持的调试与热重载；Windows 打包样例 | 不是完整 C# 调试器；Android 真机和 iOS 尚未验收 |

功能出现在样例中，不代表任意 C# 写法或任意 UE API 已受支持。[P66 实施计划](Docs/Phase66/P66.1_Implementation_Plan.md)记录剩余工作。部分冻结的 UE 调用基准优于 Puerts Reflection，但尚无整体领先 Puerts 或 Unreal AngelScript 的证据；详见[性能报告](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md)。

## 样例与文档

| 场景 | 入口 |
| --- | --- |
| Actor 生命周期与 Tick | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 道具拾取、延迟恢复 | [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| RPC、属性复制与 RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md)、[ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| C# 定义 UE 类型 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

编辑器与调试见 [IDE 工作区](Docs/Phase61/P61.D4c2_Editor_IDE_Commands.md)和[调试面板](Docs/Phase61/P61.C4b_Editor_Debugger_Panel.md)；发布见 [Windows 打包样例](Docs/Phase64/P64.D_Packaged_UI.md)和[插件安装](Docs/Phase65/P65.A_Deterministic_Release_And_Atomic_Install.md)。开发本仓库请先看 [AGENTS.md](AGENTS.md)。

## License

AvidScript 原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
