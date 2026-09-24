# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C#](https://img.shields.io/badge/C%23-WASM-512BD4?logo=dotnet&logoColor=white) ![Windows x64](https://img.shields.io/badge/Windows-x64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 把 C# 脚本编译为 WebAssembly，由 Unreal Engine 插件加载和执行。游戏运行时不加载 CLR。

目前支持 **Windows x64 + UE 5.8 源码版**；项目仍处于开发预览阶段。

![C# 源码经编译后由 UE Runtime 执行](Docs/Assets/README/pipeline.svg)

## 运行样例

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git 和 [.NET SDK 8.0.416](global.json)。将本仓库放到 C++ 项目的 `Plugins/AvidScript`，以下命令在插件目录运行。

安装 Wasmtime 依赖：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建项目的 Editor target（替换路径与 target 名称）：

```powershell
$ueRoot = "C:\Path\To\UnrealEngine"
$uproject = "C:\Path\To\YourProject.uproject"
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 Editor，在关卡中放置并选中一个 **Movable** Cube。选择 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，再点击 **Play**。Cube 会移动、旋转并放大。

只编译脚本可运行 `pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1`。如果 Play 后没有变化，检查 Actor 上的 **AvidScript Component** 和 **Output Log** 中的 `AvidScript` 错误。

## 代码

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 Tick 每秒沿 X 轴移动 Actor 120 个 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前绑定脚本的 Actor。完整文件还演示 BeginPlay、计时器、异步加载、碰撞和 EndPlay。

C# 也可以定义 UE 反射类型。以下代码摘自 [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)：

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

调用 `SetLaunchSpeedNextTick(900)` 后，属性在下一帧变为 `900`。新增或修改反射成员需要重新构建并重启 Editor；构建步骤见[类型定义样例](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 更多样例

| 样例 | 内容 |
| --- | --- |
| [PlayablePickup](Samples/CSharp/PlayablePickup/README.md) | 拾取与延迟恢复 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 与存档 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 计时器、异步加载与取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) / [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | RPC、属性复制与 RepNotify |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目中的 C++ API |

## 当前限制

| 范围 | 状态 |
| --- | --- |
| C# | 编译仓库样例使用的语言子集；不支持任意 .NET API 或 NuGet 包。标准脚本构建入口仍拒绝 `throw` / `catch` 源码；部分写法仅由专用测试编译入口覆盖。 |
| 异步 | 支持样例中的下一帧、计时器和异步资源加载；不支持任意 `Task` 或自定义 awaiter。 |
| 网络 | RPC、属性复制和 RepNotify 有样例与聚焦测试；真实游戏客户端/服务器验收尚未完成。 |
| 平台 | Windows Editor 与打包样例已验证；Android 真机和 iOS 尚未验收。 |

语言实现进度见 [P66 计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

## 仓库目录

| 目录 | 内容 |
| --- | --- |
| [`Source/`](Source/) | UE Runtime、Editor、VM 后端、绑定模块 |
| [`Tools/`](Tools/) | C# 前端、Guest IR、WASM 构建工具 |
| [`Build/`](Build/) | 依赖安装、构建与验证脚本 |
| [`Samples/`](Samples/) | 可运行样例 |
| [`Docs/`](Docs/) | 设计与验证记录 |

[实施进度](Docs/Phase66/P66.1_Implementation_Plan.md) · [性能测试条件](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md) · [仓库开发规则](AGENTS.md)

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
