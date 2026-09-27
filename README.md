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

<p align="center">
  <a href="#quick-start">快速开始</a> ·
  <a href="#installation">安装</a> ·
  <a href="#examples">示例</a> ·
  <a href="#development">构建与测试</a> ·
  <a href="#limitations">支持范围</a>
</p>

AvidScript 是 Unreal Engine 的 C# 脚本插件。源码编译为 WebAssembly，由 Wasmtime / WAMR 执行，运行时不加载 CLR。

> 开发中，当前主要测试 **UE 5.8 源码版 / Win64**。仅支持部分 C# 语法和 .NET API，详见[支持范围](#limitations)。

<a id="用法"></a>
<a id="运行示例"></a>
<a id="usage"></a>
<a id="getting-started"></a>
<a id="quick-start"></a>

## 快速开始

[安装插件](#installation)后，修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self：绑定脚本的 Actor。沿 X 轴移动，速度 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`avid_on_tick` 是插件的每帧入口。在 Editor 中：

1. 在关卡中放置并选中 Cube，设置 **Mobility → Movable**。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 开始沿 X 轴移动。

修改此示例后：停止 Play → Build And Bind → Play。

<a id="installation"></a>

## 安装

| 依赖 | 版本 / 要求 |
| --- | --- |
| Unreal Engine | 5.8 源码版 |
| Visual Studio | 2022，含 UE C++ 工具链 |
| PowerShell | 7 |
| .NET SDK | [8.0.416](global.json) |

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor，替换引擎路径、工程文件和 `MyGameEditor` target：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

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
<summary>C# 声明 UE 类型：Actor、属性与函数</summary>

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

蓝图可以继承 `Projectile`，读写 `LaunchSpeed`，调用 `GetLaunchSpeed()`。需先生成 UE 类型并编译 Editor；修改类型声明后需重新编译并重启。见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="开发"></a>
<a id="development"></a>

## 构建与测试

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

[架构](Docs/Architecture/AvidScript_Module_Architecture.md) · [语言支持](Docs/Phase66/P66.C_Language_Execution_Plan.md) · [路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)

<a id="已知限制"></a>
<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 支持范围

| 功能 | 当前限制 |
| --- | --- |
| C# / .NET | 部分语法和 API 需要单独启用；不能直接使用任意 NuGet 包。 |
| `async` / `await` | 泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`；命名 `catch` 尚未接入默认构建。 |
| 取消 | 异常对象已保存取消源身份；C# 的 `OperationCanceledException.CancellationToken` 属性尚未接入。 |
| 热重载 | 支持方法体更新；新增 UE 类型、属性、函数或修改签名后，需重新编译并重启 Editor。 |
| 平台与打包 | Shipping、Android、iOS 和完整多人游戏流程仍待验收。 |

命名 `catch` 与取消 token 的实现进度见[异步异常文档](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
