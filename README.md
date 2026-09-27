# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 源码编译为 WebAssembly，由 Wasmtime / WAMR 执行，UE 进程不加载 CLR。

> 开发中。当前主要测试 UE 5.8 / Win64；C#、.NET 和平台支持范围见[已知限制](#limitations)。

[Quick start](#quick-start) · [Installation](#installation) · [Examples](#examples) · [Development](#development) · [Limitations](#limitations)

<a id="usage"></a>
<a id="getting-started"></a>
<a id="quick-start"></a>

## Quick start

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick`：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self：绑定脚本的 Actor。沿 X 轴移动，速度 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[安装插件](#installation)后：

1. 在关卡中放置并选中 Cube，设置 **Mobility → Movable**。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. **Play**。Cube 开始沿 X 轴移动。

修改后停止 Play，重新 Build And Bind，再次 Play。

<a id="installation"></a>

## Installation

依赖：

- Unreal Engine 5.8 源码版
- Visual Studio 2022，含 UE C++ 工具链
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

在 UE C++ 工程根目录安装：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor（替换引擎路径和 `MyGame`）：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="samples"></a>
<a id="examples"></a>

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick`、输入、碰撞 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# Actor、蓝图继承、`UProperty` / `UFunction` |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 定时器与资源加载、销毁时取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端 / 服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制、`RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |

<details>
<summary>C# Actor 示例</summary>

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

蓝图可继承 `Projectile`，读写 `LaunchSpeed`，调用 `GetLaunchSpeed()`。需先生成 UE 类型并编译 Editor，见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="development"></a>

## Development

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

Roslyn 分析 C#，Guest IR 保存中间表示，WASM 后端生成 `.wasm`。运行时通过对象句柄访问 UE 对象。模块划分见[架构文档](Docs/Architecture/AvidScript_Module_Architecture.md)。

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## Limitations

- **C# / .NET**：支持部分语法和 API，不能直接使用任意 NuGet 包。部分新语法需单独启用，尚未进入默认构建。见[语言支持文档](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **异步**：泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不支持 `await`。见[异步异常文档](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。C# 暂不支持读取 [`OperationCanceledException.CancellationToken`](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。
- **热重载**：支持修改方法体。新增 UE 类型、属性、函数或修改签名，需要重新编译并重启 Editor。
- **平台**：当前主要测试 UE 5.8 / Win64。Shipping、Android、iOS 和真实多人游戏的完整验收尚未完成。

<a id="license"></a>

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
