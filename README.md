# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript: C# scripting for Unreal Engine" width="800">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-to%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# to WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor and Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

AvidScript 是 Unreal Engine 的 C# 脚本插件。编译器将 C# 编译为 WASM，UE 运行时通过生成的绑定调用引擎 API。WASM 由 Wasmtime 或 WAMR 执行，游戏进程不加载 CLR。

当前目标环境：**UE 5.8 源码版、Win64 Editor / Development**。[安装与运行](#安装与运行) · [代码示例](#代码示例) · [支持状态](#支持状态) · [开发](#开发)

## 安装与运行

需要 Visual Studio 2022 的 UE C++ 工具链、PowerShell 7、[.NET SDK 8.0.416](global.json)，以及一个 UE 5.8 源码版 C++ 工程。在**工程根目录**执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译工程的 Editor target。把 `MyGame` 和引擎路径改成自己的：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 Editor，将一个 **Movable** Cube 放入关卡并选中它，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转和缩放。改动脚本后，停止 Play，再执行一次 **Build And Bind**。

## 代码示例

下面是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`。脚本通过 `UE.Self` 访问绑定的 Actor，每秒沿 X 轴移动 120 cm：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

示例中的其他入口包括 `BeginPlay`、输入、碰撞和异步加载。按需求找代码：

| 需求 | 示例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer / latent `await`、销毁取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| C# 定义 Actor、属性和 Blueprint 函数 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| Server RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 属性复制和 `RepNotify` | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 和存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

`await` 会在 Actor 生命周期取消时停止等待（[完整示例](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)）：

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
```

`UClass` 和 `UProperty` 用来声明可供 Blueprint 使用的类型和属性（[完整示例](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)）：

```csharp
[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;
}
```

## 支持状态

| 范围 | 当前状态 |
| --- | --- |
| C# | 支持项目已实现的语法和部分 .NET API；不能直接使用任意 NuGet 包。[语言实现计划](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| 异步 | 示例中的 Timer / latent `await`、Actor 销毁取消可用；`Task<T>` 目前只支持 `Task<int>`，`catch` / `finally` 内不能 `await`。 |
| UE 类型 | 方法体变化可热重载；反射类型或签名变化需要重新编译并重启 Editor。 |
| 网络 | RPC、复制属性和 `RepNotify` 有样例及自动化验证；真实多人玩法仍需项目内测试。 |
| 平台 | 主要验证 Win64 Editor / Development；Shipping、Android、iOS 尚未验收。 |

标准 `CancellationToken` 尚未接入默认构建入口。“静态字段 + token”现可从同一 C# 源码生成已验证的 Guest IR，WASM 输出和 UE 加载仍未接通。实现进度见[能力组合合同](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## 开发

在插件根目录构建示例并运行 C# Guest 测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 源码经 Guest IR 和 WASM 进入 UE Runtime](Docs/Assets/README/pipeline.svg)

`Source/` 是 UE 模块，`Tools/` 是编译器和生成工具，`Build/` 是构建入口，`Samples/` 是示例。设计和后续工作见[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)与[迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
