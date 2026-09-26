# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

**C# scripting for Unreal Engine.**

AvidScript 将 C# 编译为 WebAssembly，在 UE 中通过 Wasmtime / WAMR 执行。支持调用 UE API、绑定 Actor 事件，以及用 C# 声明蓝图可用的类型。UE 进程不加载 CLR。

> [!NOTE]
> 实验性项目。当前开发和测试环境为 UE 5.8 / Win64；C# 与 .NET API 支持范围见 [Status](#status)。

[Installation](#installation) · [Usage](#usage) · [Samples](#samples) · [Status](#status) · [Development](#development)

## Example

用 C# 声明 Actor、蓝图属性和蓝图函数：

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

蓝图可继承 `Projectile`，调用 `SetLaunchSpeedNextTick(900.0f)` 后，`LaunchSpeed` 在下一帧更新为 `900`。
完整源码与构建说明：[ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

<a id="quick-start"></a>

## Installation

依赖：

- Unreal Engine 5.8 源码版
- Visual Studio 2022 + UE C++ 工具链
- PowerShell 7
- .NET SDK [8.0.416](global.json)（用于编译脚本）

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

在插件目录编译 Editor，替换以下工程名和引擎路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

## Usage

运行 [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 示例：

1. 打开 Editor，在关卡放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

修改示例的 `Tick`，让绑定的 Actor 沿 X 轴以 120 cm/s 移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self：当前绑定的 Actor。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

保存后再次执行 **Build And Bind**。

<a id="examples"></a>

## Samples

| 示例 | API / 用途 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入与碰撞事件 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await`、延迟执行、`EndPlay` 取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新与存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>
<a id="limitations"></a>
<a id="known-limitations"></a>

## Status

| 范围 | 当前支持 / 限制 |
| --- | --- |
| C# / .NET | 支持部分语言特性和 .NET API，不能直接使用任意 NuGet 包。见[支持清单](Docs/Phase66/P66.C_Language_Execution_Plan.md)。 |
| 静态初始化 | 静态对象字段、静态构造函数已通过同步执行测试；仅开放[编译器 API 预览](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)，尚未接入普通构建。 |
| `async` / `await` | 泛型 Task 仅支持 `Task<int>`；不支持在 `catch` / `finally` 中 `await`。 |
| `throw` / `catch` | 部分同步方法和属性访问器支持显式 `throw`，需[实验性构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)；同步异常尚不能传播到异步方法。 |
| 热重载 | 支持方法体修改；新增 UE 类型、属性、函数或修改反射签名，需重新编译并重启 Editor。 |
| 平台 | Win64 Editor / Development。Shipping、Android、iOS 和真实多人游戏验收未完成。 |

异步异常和取消的具体行为见 [Task 文档](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。

## Development

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

Roslyn 负责语义分析，Guest IR 是编译器中间表示；WASM 通过 `ObjectHandle` 访问 UE 对象。

<details>
<summary>Repository layout</summary>

```text
Source/    UE 插件模块
Tools/     C# 编译器与代码生成工具
Build/     构建与测试脚本
Samples/   脚本示例
Docs/      设计文档与测试记录
```

</details>

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
