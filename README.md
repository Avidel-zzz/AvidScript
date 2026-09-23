# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/UE-5.8-172A34?logo=unrealengine&logoColor=white) ![C#](https://img.shields.io/badge/C%23-source-512BD4?logo=dotnet&logoColor=white) ![WebAssembly](https://img.shields.io/badge/output-WASM-5541A9?logo=webassembly&logoColor=white) ![Windows x64](https://img.shields.io/badge/Windows-x64-0967A6?logo=windows&logoColor=white) ![Preview](https://img.shields.io/badge/status-preview-805413) [![MIT](https://img.shields.io/badge/license-MIT-226342)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。它把支持的 C# 源码编译成 WebAssembly，通过生成的绑定调用 UE API；游戏运行时不加载 CLR。

目前的开发和验证环境是 **UE 5.8 源码版、Windows x64**。这是 Preview 版本，只接受[支持范围](#支持范围)内的 C#；不能直接运行任意 .NET 程序或 NuGet 包。

## 代码示例

从 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 节选的 Tick 入口。`UE.Self` 是绑定脚本的 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

此处每秒沿 X 轴移动 120 个 UE 单位。完整示例还会旋转和缩放 Actor。

也可以在 C# 中声明 UE 类型。[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 中的 `Projectile` 包含以下成员（节选）：

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

`UClass`、`UProperty` 和 `UFunction` 生成对应的 UE 声明。修改声明后需重建并重启 Editor；`NextTickAsync()` 在下一帧恢复执行。

## 运行 Actor 示例

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git 和 [.NET SDK 8.0.416](global.json)。将仓库放在 C++ UE 项目的 `Plugins/AvidScript`，以下命令在该目录运行。

安装 Wasmtime 依赖：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建项目的 Editor target，替换示例中的引擎路径、项目路径和 target 名称：

```powershell
$ueRoot = "C:\Path\To\UnrealEngine"
$project = "C:\Path\To\YourProject.uproject"
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 Editor，在关卡中放置并选中一个 **Movable** Cube。运行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 应移动、旋转并放大。

只构建脚本，不绑定 Actor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

如果 Play 后没有变化，检查 Actor 上的 **AvidScript Component** 和 **Output Log** 中的 `AvidScript` 错误。

## 支持范围

| 功能 | 现有实现 | 尚未覆盖 |
| --- | --- | --- |
| C# 语言 | 控制流、数组、部分类/结构体、lambda、局部函数、受限泛型 | 任意 .NET API、NuGet；详见[语言执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| `foreach` | 数组、受限 sealed class 枚举器；同步退出时调用 `Dispose` | 其他枚举器形态、异常退出清理；[示例](Fixtures/Phase66/EnumeratorCleanup.cs) |
| `try/finally` | 同步正常结束、`return`、`break`、`continue` 的清理 | `throw`、`catch`、跨 `await` 清理；[示例](Fixtures/Phase66/FinallyCleanup.cs) |
| 异步 | 下一帧、计时器、资源加载、部分 UE Latent API | 任意 `Task` 和自定义 awaiter；[示例](Samples/CSharp/LatentGameplay/README.md) |
| UE 集成 | API 绑定、脚本定义 UE 类型、RPC/属性复制/RepNotify 聚焦测试 | 真实客户端/服务器玩法验收 |
| 平台 | Windows Editor 和打包样例 | Android 真机、iOS 验收 |

## 执行路径

![C# 编译与 UE 执行路径](Docs/Assets/README/script-to-game.png)

Roslyn 生成 Guest IR，再编译为 WASM。脚本通过句柄访问 UE 对象，不持有裸 `UObject*`。当前进度见 [P66 实施计划](Docs/Phase66/P66.1_Implementation_Plan.md)；性能数据和测试条件见[性能报告](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md)。

## 更多示例

| 用途 | 示例 |
| --- | --- |
| 拾取、延迟恢复 | [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| RPC、复制属性、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 计时器、异步加载、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |

## 仓库目录

| 路径 | 内容 |
| --- | --- |
| [`Source/`](Source/) | UE Runtime、Editor、VM 后端、绑定模块 |
| [`Tools/`](Tools/) | C# 前端、Guest IR、WASM 编译工具 |
| [`Build/`](Build/) | 依赖安装、构建和验证脚本 |
| [`Samples/`](Samples/) | 脚本示例 |
| [`Docs/`](Docs/) | 设计、验证记录和阶段计划 |

开发文档：[IDE 工作区](Docs/Phase61/P61.D4c2_Editor_IDE_Commands.md) · [调试面板](Docs/Phase61/P61.C4b_Editor_Debugger_Panel.md) · [仓库工作规则](AGENTS.md)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
