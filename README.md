# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

用 C# 写 Unreal Engine 游戏逻辑，编译为 WebAssembly 后由 UE 插件执行。游戏运行时不需要 CLR。

> 开发预览版。当前主要验证环境是 UE 5.8 源码版 + Win64；支持的 C# 语法和 UE API 以仓库样例为准。

![C# 到 UE Runtime 的编译与执行路径](Docs/Assets/README/pipeline.svg)

## 运行第一个脚本

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、Git，以及 [global.json](global.json) 指定的 .NET SDK。将仓库放到 C++ 项目的 `Plugins/AvidScript`，在该目录执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建项目的 Editor target（替换路径和 target 名称）：

```powershell
$ueRoot = "C:\Path\To\UnrealEngine"
$uproject = "C:\Path\To\YourProject.uproject"
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

在 Editor 关卡中放置并选中一个 **Movable** Cube，运行 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，然后 Play。Cube 会移动、旋转并放大。

只构建脚本可运行：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

Play 后没有变化时，检查 Actor 上的 **AvidScript Component**，并在 **Output Log** 搜索 `AvidScript`。

## C# 示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 Tick 每秒让 Actor 沿 X 轴移动 120 个 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前绑定的 Actor。完整样例还包含 BeginPlay、计时器、异步加载、碰撞和 EndPlay。

也可以在 C# 中声明 UE 反射类型。[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 中的 Actor 暴露 Blueprint 属性和函数：

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

调用 `SetLaunchSpeedNextTick(900)`，下一帧 `LaunchSpeed` 变为 `900`。新增或修改反射成员后需要重新构建并重启 Editor；见[类型定义样例](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 样例

| 功能 | 入口 | 状态 |
| --- | --- | --- |
| Actor 生命周期与 UE API | [ActorLifecycle](Samples/CSharp/ActorLifecycle/) | Win64 Editor 样例 |
| 计时器、下一帧与异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 样例与自动化测试 |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md)、[ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 聚焦测试；真实客户端/服务器验收待做 |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 样例 |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 生成绑定与样例 |
| C# 定义 UE 类型 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 样例与自动化测试 |

当前限制：

- 编译器只支持已实现的 C# 子集；不能直接运行任意 .NET 项目或 NuGet 包。
- `throw` / `catch` / `finally` 仅在专用测试编译入口支持零参 `System.Exception`、部分同步清理和重抛；`catch` 变量只支持引用判断。标准脚本构建入口仍拒绝异常源码。详见 [P66 计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- Windows Editor 与打包样例已有验证；Android 真机与 iOS 尚未验收。

## 目录

`Source/` UE 插件与 Runtime · `Tools/` C# 前端和 WASM 工具 · `Build/` 构建与验证脚本 · `Samples/` 可运行样例 · `Docs/` 设计与验证记录

开发约定见 [AGENTS.md](AGENTS.md)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
