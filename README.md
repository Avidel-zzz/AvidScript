# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# → WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor / Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

AvidScript 为 Unreal Engine 提供 C# 脚本支持。C# 编译为 WebAssembly，通过生成的绑定调用 UE API，运行在 Wasmtime / WAMR 中。UE 进程不加载 CLR。

> 开发中。当前以 UE 5.8 源码版、Win64 Editor / Development 为测试环境，仅支持部分 C# 语法和 .NET API。见[已知限制](#limitations)。

[Quick start](#quick-start) · [Examples](#examples) · [Build & test](#development) · [Limitations](#limitations) · [Docs](#docs)

<a id="用法"></a>
<a id="usage"></a>

例如，在 `Tick` 中移动脚本绑定的 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(
        new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向当前 Actor；`avid_on_tick` 每帧调用一次。上面的代码让 Actor 沿 X 轴以 120 cm/s 移动。

<a id="getting-started"></a>
<a id="quick-start"></a>

## Quick start

### Requirements

- Unreal Engine 5.8 源码版、UE C++ 工程
- Visual Studio 2022，已安装 UE C++ 工具链
- PowerShell 7、[.NET SDK 8.0.416](global.json)

<a id="installation"></a>

### Install

在 UE 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译工程的 Editor target。以下路径和 `MyGameEditor` 按自己的工程修改：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="运行示例"></a>

### Run

1. 将 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 替换为上面的代码。
2. 打开 Editor，在关卡中选中一个 Cube，设置 **Mobility → Movable**。
3. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，构建成功后点击 **Play**。

修改此示例后，停止 Play，再次执行 **Build And Bind**，然后 Play。

<a id="示例目录"></a>
<a id="samples"></a>
<a id="examples"></a>

## Examples

| 功能 | 示例 |
| --- | --- |
| `BeginPlay` / `Tick`、输入与碰撞回调 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| `[UClass]` / `[UProperty]` / `[UFunction]`、蓝图继承 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| `await` 等待定时器、异步加载资源 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| 客户端与服务器 RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 属性复制与 `RepNotify` 回调 | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 更新与存档读写 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 生成项目 C++ API 的 C# 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

<details>
<summary><code>UClass</code> 示例</summary>

```csharp
using AvidScript;

namespace AvidScriptSamples;

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

生成 UE 类型并编译 Editor 后，蓝图可继承 `Projectile`，读写 `LaunchSpeed` 并调用 `GetLaunchSpeed()`。类型声明变化需要重新编译并重启 Editor，步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="开发"></a>
<a id="development"></a>

## Build & test

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

编译器在 `Tools/`，UE 插件模块在 `Source/`，构建脚本在 `Build/`。

<a id="已知限制"></a>
<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## Limitations

| 功能 | 当前限制 |
| --- | --- |
| C# / .NET | 尚未覆盖完整 C# 语法与 .NET 标准库，不能直接使用任意 NuGet 包。 |
| `async` / `await` | 泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`。 |
| 异常与取消 | 默认构建暂不支持在 `async` 方法中使用 `catch (Exception error)` 的异常变量；尚不支持读取 `OperationCanceledException.CancellationToken`。 |
| 热重载 | 支持修改方法体；新增 UE 类型、属性、函数或修改签名，需要重新编译并重启 Editor。 |
| 平台与打包 | 当前主要测试 Win64 Editor / Development。Shipping、Android、iOS 和完整多人游戏流程仍待验收。 |

异常与取消的实现进度见[异步异常文档](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。

<a id="docs"></a>

## Docs

![C# 编译为 WASM，再由 UE 运行时加载并访问 UObject](Docs/Assets/README/pipeline.svg)

- [模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)
- [C# 语法与执行支持](Docs/Phase66/P66.C_Language_Execution_Plan.md)
- [开发路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
