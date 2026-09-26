# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。C# 通过 Roslyn 编译为 WebAssembly，在 UE 中执行，不依赖 CLR。

目前为开发预览版，主要测试环境是 **UE 5.8 / Win64 Editor / Development**。使用前请查看[已知限制](#limitations)。

[快速开始](#installation) · [示例](#examples) · [已知限制](#limitations) · [构建与测试](#development)

## Usage

在 C# 中处理 Actor 的 Tick：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 沿 X 轴移动，速度 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是挂载脚本的 Actor，`avid_on_tick` 对应 UE 的 Tick 回调。
这段代码可替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 方法；安装后可直接运行该示例。

<a id="快速开始"></a>
<a id="安装"></a>
<a id="quick-start"></a>

## Installation

<a id="requirements"></a>

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工具）、PowerShell 7 和 .NET SDK [8.0.416](global.json)。

<a id="install"></a>

在 UE 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建 Editor。将引擎路径、`MyGame.uproject` 和 `MyGameEditor` 替换为自己的配置：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="run-the-sample"></a>

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

要修改脚本，编辑 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs`，然后重新执行 **Build And Bind**。

<a id="示例"></a>

## Examples

| 示例 | 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 处理 `BeginPlay`、`Tick`、`EndPlay`、输入和碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 用 `await` 等待延迟，用取消令牌结束等待 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 在客户端和服务器之间调用 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，在 `RepNotify` 中处理更新 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 更新 UI、读写存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 声明 Actor、Component、Subsystem |

C# 声明 Actor，并向蓝图暴露属性和方法（节选自 [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)）：

```csharp
using AvidScript;

namespace AvidScriptSamples;

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

生成并编译这个类型后，蓝图可以继承 `Projectile`、读写 `LaunchSpeed`，或调用 `SetLaunchSpeedNextTick` 在下一帧修改速度。等待不需要另外实现 `Tick`。修改类型声明需要重新构建 Editor。

<a id="当前边界"></a>
<a id="已知限制"></a>
<a id="支持范围"></a>

## Limitations

| 范围 | 限制 |
| --- | --- |
| C# / .NET | 支持部分 C# 语法，不能直接运行任意 .NET 程序或 NuGet 包。[语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| `async` / `await` | Task 值限于 `Task<int>`；`catch`、`finally` 中不能使用 `await`。[异步支持范围与构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build) |
| UE 类型声明 | 修改 `[UClass]`、`[UProperty]`、`[UFunction]` 声明后，需要重新构建并重启 Editor。 |
| 平台与发布 | Android、iOS、Shipping 和真实多人游戏尚未完成验收。 |

生成 UE 类型中的 Task 异常与取消流程仍属预览功能，需要通过构建参数显式启用。[取消过程中显式抛错](Docs/Phase66/P66.C9_Async_Throw_Routing.md)的组合尚不能编译运行。

<a id="构建与测试"></a>
<a id="开发"></a>
<a id="build-and-test"></a>
<a id="build--test"></a>

## Development

以下命令均在 `Plugins/AvidScript` 下执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<a id="项目结构"></a>
<a id="internals"></a>
<a id="layout"></a>

<a id="architecture"></a>

<details>
<summary>源码结构与编译流程</summary>

| 目录 | 内容 |
| --- | --- |
| `Source/` | UE 插件模块 |
| `Tools/` | C# 编译器、WASM 生成工具 |
| `Build/` | 构建与测试脚本 |
| `Samples/` | 示例脚本 |
| `Docs/` | 设计文档与测试记录 |

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

Roslyn 分析 C# 源码，编译器将中间表示 Guest IR 转换为 WASM。UE 运行时加载 WASM；脚本通过 `ObjectHandle` 句柄访问 UE 对象。

</details>

<a id="许可"></a>

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
