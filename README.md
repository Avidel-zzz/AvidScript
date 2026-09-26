# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。使用 Roslyn 分析 C#，编译为 WebAssembly，由 Wasmtime / WAMR 执行；UE 运行时不加载 CLR。

支持 UE API 调用、Actor 事件、异步任务、RPC，以及 C# 定义的 Actor、Component 和 Subsystem。

**状态：开发中。** 当前开发与测试环境为 UE 5.8 / Win64。C# 和 .NET API 尚未完整支持，详见 [Limitations](#limitations)。

[Quick start](#quick-start) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

每帧移动 Actor（120 cm/s）：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向当前脚本绑定的 Actor，`avid_on_tick` 是每帧回调入口。替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 即可使用。

## Quick start

### Requirements

- Unreal Engine 5.8 源码版
- Visual Studio 2022，含 UE C++ 工具链
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

### Installation

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor target（替换 `MyGame` 和引擎路径）：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

### Usage

打开 Editor：

1. 在关卡中放置 Cube，设置 `Mobility = Movable`，保持选中。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建完成后点击 **Play**，Cube 开始移动、旋转和缩放。

脚本位于 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs`。修改后通过同一菜单重新构建并绑定。

<a id="samples"></a>

## Examples

| 示例 | API / 功能 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入与碰撞事件 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 定时等待与取消：`DelayAsync`、`WithCancellation` |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC（网络调用） |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 项目 C++ API 的 C# 绑定 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 用 `[UClass]`、`[UProperty]`、`[UFunction]` 声明 UE 类型与成员 |

<details>
<summary>示例：用 C# 定义蓝图可继承的 Actor</summary>

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

生成并编译后，蓝图可继承 `Projectile`、读写 `LaunchSpeed`、调用 `GetLaunchSpeed()`。构建步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>

## Limitations

- **C# / .NET**：仅支持[语言支持清单](Docs/Phase66/P66.C_Language_Execution_Plan.md)中的语法与 API，不能直接使用任意 NuGet 包。
- **异步**：泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 中不能 `await`。见[异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- **热重载**：支持修改方法体。新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。
- **平台**：Shipping、Android、iOS 和真实多人游戏验收未完成。

<details>
<summary>实验性编译器功能</summary>

| 功能 | 限制 |
| --- | --- |
| 静态字段初始化、静态构造函数 | 仅[编译器 API](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)可用，普通构建脚本尚未接入 |
| 同步方法、属性访问器中的显式 `throw` | 需开启[实验性参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)，仅支持部分用法 |
| 同步异常传入调用方的 `async` 方法 | [编译器 API 预览](Docs/Phase66/P66.C10_Synchronous_Error_Task_Transfer.md#c-源码到-vm-的执行验证)已通过 Win64 测试；成员 `await` 写回与静态初始化组合尚未支持 |

</details>

## Development

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

运行时通过 `ObjectHandle`（对象句柄）访问 UE 对象，不向脚本暴露 `UObject*`。

```text
Source/    UE 插件模块
Tools/     C# 编译器与代码生成工具
Build/     构建与测试脚本
Samples/   脚本示例
Docs/      设计文档与测试记录
```

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
