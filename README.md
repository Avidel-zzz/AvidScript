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

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，通过生成的绑定调用 UE API；运行时使用 Wasmtime / WAMR，不加载 CLR。

**开发中** · UE 5.8 源码版 · Win64 · 支持部分 C# / .NET API

[安装](#installation) · [用法](#usage) · [示例](#examples) · [开发](#development) · [已知限制](#limitations)

<a id="installation"></a>

## 安装

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7、[.NET SDK 8.0.416](global.json)。

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor。将下列路径和 `MyGameEditor` 替换为自己的工程配置：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="用法"></a>
<a id="运行示例"></a>
<a id="usage"></a>
<a id="getting-started"></a>
<a id="quick-start"></a>

## 用法

以 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 为例，将 `Tick` 改为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor，`avid_on_tick` 每帧执行一次。这段代码让 Actor 沿 X 轴以 120 cm/s 移动。

1. 打开 Editor，在关卡中选中一个 Cube，设置 **Mobility → Movable**。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。

重新编译此示例：停止 Play → 修改源码 → Build And Bind → Play。

<a id="示例目录"></a>
<a id="samples"></a>
<a id="examples"></a>

## 示例

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick`、输入、碰撞 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | `[UClass]`、`[UProperty]`、`[UFunction]`、蓝图继承 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 等待定时器、异步加载资源 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |

<details>
<summary>示例：用 C# 声明可被蓝图继承的 Actor</summary>

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

生成 UE 类型并编译 Editor 后，蓝图可继承 `Projectile`、读写 `LaunchSpeed`、调用 `GetLaunchSpeed()`。修改类型声明需要重新编译并重启 Editor。构建步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="开发"></a>
<a id="development"></a>

## 开发

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

```text
Source/    UE 插件模块
Tools/     C# 编译器、代码生成器与测试
Build/     构建与验证脚本
Samples/   示例源码
Docs/      使用说明与设计文档
```

[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md) · [C# 支持进度](Docs/Phase66/P66.C_Language_Execution_Plan.md) · [路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)

<a id="已知限制"></a>
<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 已知限制

| 功能 | 当前限制 |
| --- | --- |
| C# / .NET | 尚未覆盖完整 C# 语法与 .NET 标准库，不能直接使用任意 NuGet 包。 |
| `async` / `await` | 泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`。 |
| 异常与取消 | 默认构建暂不支持在 `async` 方法中使用 `catch (Exception error)` 的异常变量；尚不支持读取 `OperationCanceledException.CancellationToken`。 |
| 热重载 | 支持修改方法体；新增 UE 类型、属性、函数或修改签名，需要重新编译并重启 Editor。 |
| 平台与打包 | 当前主要测试 Win64 Editor / Development。Shipping、Android、iOS 和完整多人游戏流程仍待验收。 |

异常变量与取消 token 的详细支持情况见[异步异常文档](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
