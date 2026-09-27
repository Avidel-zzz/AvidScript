# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript: C# scripting for Unreal Engine" width="800">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# to WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor and Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

在 Unreal Engine 中运行 C# 脚本：编译器将 C# 转为 WASM，插件通过生成的绑定调用 UE API。运行时支持 Wasmtime 和 WAMR，不在 UE 进程中加载 CLR。

当前面向 **UE 5.8 源码版 / Win64 Editor** 开发；语言、平台和发布支持范围见[当前边界](#当前边界)。

[Quick start](#quick-start) · [Examples](#examples) · [Current support](#current-support) · [Development](#development) · [Docs](#docs)

## Usage

下面摘自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。`UE.Self` 是当前绑定脚本的 Actor；`avid_on_tick` 每帧执行，代码以 120 cm/s 沿 X 轴移动它。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

这是方法片段；完整脚本还包含 `BeginPlay`、输入、碰撞和异步加载回调。

<a id="getting-started"></a>
<a id="installation"></a>

## Quick start

需要 UE 5.8 源码版的 C++ 工程、Visual Studio 2022 UE C++ 工具链、PowerShell 7，以及仓库 [global.json](global.json) 指定的 .NET SDK 8.0.416。

在 **UE 工程根目录**安装插件：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译工程的 Editor target。将示例中的引擎路径、工程名和 target 名替换为自己的：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

在 Editor 中运行仓库自带示例：

1. 在关卡里选中 Cube，并将 **Mobility** 设为 **Movable**。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。Cube 会移动、旋转并缩放。

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，停止 Play，重新执行 **Build And Bind**，再进入 Play。

<a id="samples"></a>

## Examples

![玩法 Tick、异步等待和脚本定义蓝图类型的三个入口](Docs/Assets/README/three-ways-to-start.svg)

| 想做什么 | 对应代码 / 示例 |
| --- | --- |
| 在 `BeginPlay` / `Tick` 中控制 Actor，处理输入和碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| `await` 等待定时器，并在 Actor 销毁时取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| 声明可供蓝图使用的 Actor、属性和函数 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) |
| 从客户端调用 Server RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 同步属性并执行 `RepNotify` | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| 更新 UI、读写存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 为项目自己的 C++ API 生成 C# 调用接口 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

例如，[LatentGameplay](Samples/CSharp/LatentGameplay/README.md) 中等待 UE latent 函数，并把等待绑定到 Actor 生命周期：

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
```

完整的 `BeginPlay`、取消源创建和 `EndPlay` 清理在示例中。

用 C# 声明 UE 类型的片段来自 [Projectile](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)：

```csharp
[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    [UFunction(BlueprintPure = true, Category = "Projectile")]
    public float GetLaunchSpeed()
    {
        return LaunchSpeed;
    }
}
```

蓝图子类可读写 `LaunchSpeed` 并调用 `GetLaunchSpeed()`；新增或修改反射成员后需要重新编译并重启 Editor。

<a id="当前边界"></a>
<a id="limitations"></a>

## Current support

| 范围 | 当前状态 |
| --- | --- |
| 开发环境 | UE 5.8 源码版、Win64 Editor / Development。 |
| C# | 支持项目实现的语法与部分 .NET API；不能直接使用任意 NuGet 包。 |
| UE 类型 | 可通过 `[UClass]`、`[UProperty]`、`[UFunction]` 生成类型。方法体可热重载；类型或签名变化需要重编译并重启 Editor。 |
| 异步 | 支持示例中的 Timer / latent / 资源加载等待；泛型 Task 目前仅支持 `Task<int>`，不能在 `catch` / `finally` 内 `await`。 |
| 异常与取消 | 显式编译入口的 `CancellationToken` 属性已通过双 VM 专项测试；默认构建入口尚未开放，不能直接按完整 .NET 支持使用。见[实现进度](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。 |
| 网络 | RPC、属性复制和 `RepNotify` 有专项样例与自动化；真实项目的多人玩法仍需自行验收。 |
| 发布平台 | Shipping、Android、iOS 尚未完成验收。 |

## Development

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行 C# 编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

```text
Source/    UE Runtime、VM、Bindings、Editor 模块
Tools/     C# 编译器、代码生成器和测试
Build/     构建与验证入口
Samples/   可阅读的示例源码
Docs/      架构、支持范围和阶段记录
```

<a id="docs"></a>

## Docs

![C# 源码经编译器生成 WASM，由 UE 运行时加载并访问 UObject](Docs/Assets/README/pipeline.svg)

- [模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)
- [C# 支持范围与实现进度](Docs/Phase66/P66.C_Language_Execution_Plan.md)
- [开发路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) 和 [WAMR](Source/ThirdParty/WAMR/README.md)；Unreal Engine 不包含在本仓库中。
