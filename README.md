# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/UE-5.8-172A34?logo=unrealengine&logoColor=white) ![C#](https://img.shields.io/badge/C%23-source-512BD4?logo=dotnet&logoColor=white) ![WebAssembly](https://img.shields.io/badge/output-WASM-5541A9?logo=webassembly&logoColor=white) ![Windows x64](https://img.shields.io/badge/Windows-x64-0967A6?logo=windows&logoColor=white) ![Preview](https://img.shields.io/badge/status-preview-805413) [![MIT](https://img.shields.io/badge/license-MIT-226342)](LICENSE)

Unreal Engine 5.8 的 C# 脚本插件。构建工具把受支持的 C# 编译成 WebAssembly，UE 插件加载并执行脚本；游戏进程不需要 CLR。

当前版本面向 **Windows x64 + UE 5.8 源码版**。语言和平台限制见[支持范围](#支持范围)。

## 快速开始

需要 Windows 10/11 x64、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git 和 [.NET SDK 8.0.416](global.json)。将仓库放在 C++ UE 项目的 `Plugins/AvidScript`，在该目录运行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建 Editor target；替换下面的路径和 target 名称：

```powershell
$ueRoot = "C:\Path\To\UnrealEngine"
$project = "C:\Path\To\YourProject.uproject"
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 Editor，在关卡中放置一个 **Movable** Cube 并选中它。执行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，再点击 **Play**。Cube 会移动、旋转并放大。没有变化时，检查 Actor 上的 **AvidScript Component** 和 **Output Log** 中的 `AvidScript` 错误。

只构建脚本、不绑定 Actor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 代码示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 Tick 入口每秒沿 X 轴移动 Actor 120 个 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向绑定脚本的 Actor。完整文件还包含 BeginPlay、计时器、异步加载、碰撞和 EndPlay。

C# 也能声明 UE 类型。下面摘自 [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)：

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

调用 `SetLaunchSpeedNextTick(900)` 后，`LaunchSpeed` 在下一帧变为 `900`。新增属性或函数需要重新构建并重启 Editor；具体构建方式见[脚本定义 UE 类型样例](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 示例项目

| 示例 | 演示内容 |
| --- | --- |
| [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) | 拾取道具、延迟恢复 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI、存档 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 计时器、异步加载、取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | RPC、属性复制、RepNotify |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目自己的 C++ API |

## 支持范围

- **C#：** 支持仓库样例使用的语言子集；不支持任意 .NET API 或 NuGet 包。`throw` / `catch` 目前只有专用测试编译入口覆盖部分写法，标准脚本构建入口仍会拒绝异常源码，见 [P66 语言执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **UE 集成：** 通过生成的绑定访问 UE API，也可用 C# 声明 Actor、Component 和 Subsystem。新增或修改 UE 反射成员需要重新构建并重启 Editor。
- **异步：** 支持样例中的下一帧、计时器和异步资源加载；不支持任意 `Task` 或自定义 awaiter。
- **网络：** RPC、属性复制和 RepNotify 有聚焦测试及样例；真实游戏客户端/服务器验收仍需完成。
- **平台：** 已在 Windows Editor 和打包样例中验证；Android 真机与 iOS 尚未验收。

## 项目结构

![C# 到 UE 的执行路径](Docs/Assets/README/script-to-game.png)

`C#` → [Roslyn 前端](Tools/AvidScript.CSharpFrontend/) → [Guest IR](Tools/AvidScript.GuestIr/) → `WASM` → [UE Runtime](Source/AvidScriptRuntime/)。脚本通过句柄访问 UE 对象。

| 目录 | 内容 |
| --- | --- |
| [`Source/`](Source/) | UE Runtime、Editor、VM 后端、绑定模块 |
| [`Tools/`](Tools/) | C# 前端、Guest IR、WASM 构建工具 |
| [`Build/`](Build/) | 依赖安装、构建和验证入口 |
| [`Samples/`](Samples/) | 可运行脚本样例 |
| [`Docs/`](Docs/) | 设计、实施计划和验证记录 |

[实施进度](Docs/Phase66/P66.1_Implementation_Plan.md) · [性能报告与测试条件](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md) · [仓库工作规则](AGENTS.md)

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
