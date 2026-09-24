# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是面向 Unreal Engine 的 C# 脚本插件。C# 源码编译为 WebAssembly，再由 UE 插件加载执行；游戏运行时不加载 CLR。

**状态：开发预览版。** 当前开发和验证以 UE 5.8 源码版、Windows x64 为主。语言只支持仓库样例覆盖的 C# 子集，不能直接运行任意 .NET 项目。

![C# 源码经编译后由 UE Runtime 执行](Docs/Assets/README/pipeline.svg)

## 快速开始

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git 和 [.NET SDK 8.0.416](global.json)。把本仓库放在 C++ 项目的 `Plugins/AvidScript` 目录下，以下命令均从该目录运行。

1. 安装 Wasmtime 依赖：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建项目的 Editor target（替换引擎、项目路径和 target 名称）：

   ```powershell
   $ueRoot = "C:\Path\To\UnrealEngine"
   $uproject = "C:\Path\To\YourProject.uproject"
   & "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
     YourProjectEditor Win64 Development "-Project=$uproject" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 在 Editor 关卡中放置并选中一个 **Movable** Cube，执行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。脚本会移动、旋转并放大 Cube。

只编译脚本可运行 `pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1`。Play 后没有变化时，检查 Actor 上的 **AvidScript Component**，并在 **Output Log** 搜索 `AvidScript`。

## 代码示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 Tick 沿 X 轴以每秒 120 个 UE 单位移动 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向绑定脚本的 Actor。完整样例还包含 BeginPlay、计时器、异步加载、碰撞和 EndPlay。

C# 也可以声明 UE 反射类型。[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 中的 Actor 定义了 Blueprint 可访问的属性和函数：

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

调用 `SetLaunchSpeedNextTick(900)` 后，下一帧的 `LaunchSpeed` 为 `900`。新增或修改反射成员需要重新构建并重启 Editor；完整步骤见[类型定义样例](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 样例与支持范围

| 场景 | 样例 | 当前边界 |
| --- | --- | --- |
| Actor 生命周期 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/) | BeginPlay、Tick、EndPlay 与 UE API 调用 |
| 异步 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 下一帧、计时器、异步资源加载；不支持任意 `Task` 或自定义 awaiter |
| 网络 | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md)、[ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | RPC、属性复制和 RepNotify 有聚焦测试；真实游戏客户端/服务器验收待做 |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 以样例覆盖的 API 为准 |
| 项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 通过生成的绑定调用 |

编译器目前只支持受控 C# 子集，不支持任意 .NET API 或 NuGet 包。受限 `throw` / `catch`（含嵌套 `throw;` 重抛）和线性同步 `finally` 目前只在专用测试编译入口覆盖；标准脚本构建入口仍拒绝异常源码。Windows Editor 与打包样例已有验证；Android 真机和 iOS 尚未验收。语言侧正在进行的工作见 [P66 计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

## 仓库目录

- [`Source/`](Source/)：UE Runtime、Editor、VM 后端和绑定模块。
- [`Tools/`](Tools/)：C# 前端、Guest IR 和 WASM 构建工具。
- [`Build/`](Build/)：依赖安装、构建和验证脚本。
- [`Samples/`](Samples/)：可运行样例。
- [`Docs/`](Docs/)：设计、实施计划和验证记录。

开发约定见 [AGENTS.md](AGENTS.md)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
