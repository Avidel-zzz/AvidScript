# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/UE-5.8-172A34?logo=unrealengine&logoColor=white) ![C#](https://img.shields.io/badge/C%23-source-512BD4?logo=dotnet&logoColor=white) ![WebAssembly](https://img.shields.io/badge/output-WASM-5541A9?logo=webassembly&logoColor=white) ![Windows x64](https://img.shields.io/badge/Windows-x64-0967A6?logo=windows&logoColor=white) ![Preview](https://img.shields.io/badge/status-preview-805413) [![MIT](https://img.shields.io/badge/license-MIT-226342)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。脚本编译为 WebAssembly；UE 运行时不加载 CLR。

**当前状态：** Windows x64 预览版。接入项目前请查看[支持情况](#支持情况)。

## 示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 Tick 每帧读取并更新 Actor 位置：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前绑定的 Actor。完整样例还包含 BeginPlay、计时器、异步加载和碰撞事件。

## 快速开始

要求：Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git、[.NET SDK 8.0.416](global.json)。将仓库放到 C++ 项目的 `Plugins/AvidScript`，以下命令均在该目录执行。

1. 安装 Wasmtime 依赖：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建 Editor target。把路径和 `YourProjectEditor` 换成自己的项目：

   ```powershell
   $ueRoot = "C:\Path\To\UnrealEngine"
   $project = "C:\Path\To\YourProject.uproject"
   & "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
     YourProjectEditor Win64 Development "-Project=$project" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 打开 Editor，放置并选中一个 **Movable** Cube。运行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。

只编译脚本可运行 `pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1`。如果 Play 后没有变化，检查 Actor 上的 **AvidScript Component** 和 **Output Log** 中的 `AvidScript` 错误。

## 支持情况

| 功能 | 示例 / 说明 | 当前限制 |
| --- | --- | --- |
| C# 游戏逻辑 | [Actor 生命周期](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)、[拾取玩法](Samples/CSharp/PlayablePickup/README.md) | 只编译受支持的 C# 子集；不支持任意 .NET API 或 NuGet 包 |
| UE 类型声明 | [Actor、Component、Subsystem](Samples/CSharp/ScriptDefinedTypes/README.md) | 声明或签名变化需要重新构建并重启 Editor |
| 异步 | [计时器、异步加载、取消](Samples/CSharp/LatentGameplay/README.md) | 不支持任意 `Task` 或自定义 awaiter |
| 网络 | [RPC](Samples/CSharp/NetworkRpc/README.md)、[属性复制 / RepNotify](Samples/CSharp/ReplicatedProperty/README.md) | 真实游戏的客户端/服务器验收仍需完成 |
| 控制流清理 | [`foreach` 清理](Fixtures/Phase66/EnumeratorCleanup.cs)、[`finally` 清理](Fixtures/Phase66/FinallyCleanup.cs) | 公开构建入口仍不支持 `throw`/`catch` 和跨 `await` 的异常清理 |
| 平台 | Windows Editor 与打包样例 | Android 真机和 iOS 尚未验收 |

完整语言范围与待办见 [P66 语言执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

## 实现与文档

![C# 到 UE 的执行路径](Docs/Assets/README/script-to-game.png)

Roslyn → [Guest IR](Tools/AvidScript.GuestIr/) → WASM → [UE Runtime](Source/AvidScriptRuntime/)。UE 对象通过句柄访问。源码和更多样例入口：

| 路径 | 内容 |
| --- | --- |
| [`Source/`](Source/) | UE Runtime、Editor、VM 后端和绑定模块 |
| [`Tools/`](Tools/) | C# 前端、Guest IR、WASM 编译器 |
| [`Build/`](Build/) | 依赖安装、构建和验证脚本 |
| [`Samples/`](Samples/) | UI/存档、网络、UE 类型和玩法样例 |

[开发进度](Docs/Phase66/P66.1_Implementation_Plan.md) · [性能报告与测试条件](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md) · [仓库工作规则](AGENTS.md)

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
