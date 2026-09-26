# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。使用 Roslyn 编译 C#，通过 Wasmtime / WAMR 运行 WebAssembly，无需在 UE 中托管 CLR。

当前为开发预览版，主要支持 UE 5.8 / Win64。C# 和 .NET API 支持范围见 [Limitations](#limitations)。

[Installation](#installation) · [Usage](#usage) · [Examples](#examples) · [Development](#development) · [Limitations](#limitations)

替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`：

```csharp
// 每帧调用；UE.Self 指向绑定脚本的 Actor
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 沿 X 轴移动，速度 120 cm/s
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

<a id="installation"></a>

## Installation

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7、[.NET SDK 8.0.416](global.json)。

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor target，按项目修改 `$ueRoot`、`MyGame.uproject` 和 `MyGameEditor`：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>
<a id="getting-started"></a>
<a id="quick-start"></a>

## Usage

1. 打开 Editor，在关卡中放置 Cube，设为 `Movable` 并选中。
2. 选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建完成后点击 **Play**。

默认脚本会移动、旋转和缩放 Cube。修改脚本后：停止 Play → 重新执行构建菜单 → Play。

<a id="samples"></a>
<a id="examples"></a>

## Examples

### Actor / Blueprint

[ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) 节选。声明 Actor、蓝图属性和纯函数：

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

生成并编译后，可创建 `Projectile` 的蓝图子类，在蓝图中读写 `LaunchSpeed`、调用 `GetLaunchSpeed()`。构建说明见示例 README。

### Samples

| 示例 | 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await`、定时器、销毁时取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 项目 C++ API 绑定 |

<a id="development"></a>

## Development

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

<details>
<summary>目录结构</summary>

```text
Source/     UE 插件模块
Tools/      C# 编译器与代码生成工具
Build/      构建与测试脚本
Samples/    脚本示例
Docs/       设计文档与测试记录
```

</details>

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## Limitations

- **C# / .NET**：仅支持部分语法和 API，不能直接引用任意 NuGet 包。见[语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **Async**：泛型 Task 仅支持 `Task<int>`；`catch`、`finally` 内不能 `await`。见[异步支持范围](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- **热重载**：支持修改方法体。新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。
- **平台与发布**：Shipping、Android、iOS 和真实多人游戏的完整验证尚未完成。

<details>
<summary>实验性功能</summary>

以下用法需要编译器 API 或额外构建参数，尚未全部接入默认构建命令：

| 用法 | 文档 |
| --- | --- |
| `static int Count = InitCount();` | [静态字段初始化与对象生命周期](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md) |
| 同步方法和属性访问器中的 `throw` | [支持范围与构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build) |
| `target.Value = await GetValueAsync();` | [异步成员赋值](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证)，暂不支持接口属性和生成的 UE 类型 |

`NextTickAsync().WithCancellation(token)` 已支持预取消时同步完成，见[验证范围](Docs/Phase66/P66.C10_PreCancelled_Await_Contract.md)。

</details>

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
