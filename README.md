# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，由 Wasmtime / WAMR 执行；UE 运行时不依赖 CLR。

开发预览版。目前在 **UE 5.8 / Win64** 上开发和测试，支持部分 C# 语法与 .NET API。

[Getting started](#getting-started) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

```csharp
// ActorLifecycleScript.cs — 每帧沿 X 轴移动，速度 120 cm/s。
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向绑定脚本的 Actor。上面的 `Tick` 可替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的同名方法。

<a id="quick-start"></a>
<a id="installation"></a>

## Getting started

### Requirements

- Unreal Engine 5.8 源码版
- Visual Studio 2022，含 UE C++ 工具链
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

### Build

在 UE C++ 工程根目录克隆插件并安装 Wasmtime：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor target。`MyGame` 为工程名，`$ueRoot` 为源码引擎目录：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

### Run

1. 打开 Editor，在关卡中放置一个 Cube，设为 `Movable` 并选中。
2. 选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建完成后点击 **Play**。默认示例会移动、旋转和缩放 Cube。

编辑 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs`，再次执行该菜单重建脚本。

<a id="samples"></a>

## Examples

| 示例 | 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 移动 Actor，处理输入与碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 定时等待、取消任务 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 调用 Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，在 `RepNotify` 中处理更新 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 定义 Actor、Component、Subsystem，供蓝图继承和调用 |

### Define a UE type

通过 `[UClass]`、`[UProperty]`、`[UFunction]` 声明 UE 类型、属性和函数：

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

生成并编译后，可创建 `Projectile` 的蓝图子类。`LaunchSpeed` 可在蓝图中读写，`GetLaunchSpeed()` 对应一个纯函数节点。构建与运行步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>

## Limitations

- C# / .NET：不能直接引用任意 NuGet 包。支持范围见[语言支持清单](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- `async` / `await`：泛型 Task 仅支持 `Task<int>`；不支持在 `catch` / `finally` 中 `await`。见[异步异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- 热重载支持修改方法体。新增 UE 类型、属性、函数或修改反射签名后，需要重新编译并重启 Editor。
- Shipping、Android、iOS 和真实多人游戏尚未完成验收。

<details>
<summary>Experimental compiler features</summary>

以下功能需使用编译器 API 或实验性参数，默认构建流程未全部接入：

| 功能 | 当前限制 |
| --- | --- |
| [静态字段初始化、静态构造函数](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md) | 普通构建脚本未接入 |
| [同步方法、属性访问器中的 `throw`](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build) | 部分用法可用，需启用实验性参数 |
| [同步异常传递到 `async` 调用方](Docs/Phase66/P66.C10_Synchronous_Error_Task_Transfer.md#c-源码到-vm-的执行验证) | 已通过 Win64 测试；`target.Value = await ReadAsync()` 尚未完成，与静态初始化的组合用法尚不支持 |

</details>

## Development

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

编译器使用 Roslyn 分析 C#，经 Guest IR（中间表示）生成 WASM。运行时通过 `ObjectHandle`（对象句柄）访问 UE 对象：

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

```text
Source/     UE 插件模块
Tools/      C# 编译器与代码生成工具
Build/      构建与测试脚本
Samples/    脚本示例
Docs/       设计文档与测试记录
```

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
