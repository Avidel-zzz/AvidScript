# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件，使用 Roslyn 编译到 WebAssembly，运行于 Wasmtime / WAMR，不依赖 CLR。

> **Development preview** · UE 5.8 · Win64 Editor / Development。C# 和 .NET 支持范围见 [Limitations](#limitations)。

[Installation](#installation) · [Usage](#usage) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

## Example

C# 声明 Actor、蓝图属性和函数：

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

蓝图可继承 `Projectile`，读写 `LaunchSpeed`，调用 `GetLaunchSpeed()`。[完整源码](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) · [构建方法](Samples/CSharp/ScriptDefinedTypes/README.md)

<a id="quick-start"></a>

## Installation

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7、[.NET SDK 8.0.416](global.json)。

在 UE C++ 工程根目录安装：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

在插件目录编译 Editor。将 `MyGame` 和引擎路径替换为本地配置：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

## Usage

将 [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 绑定到关卡中的 Actor：

1. 放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。Cube 会移动、旋转和缩放。

修改示例中的 `Tick`，可调整每帧行为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 当前 Actor 沿 X 轴移动，速度 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

保存后再次执行 **Build And Bind**。

<a id="samples"></a>

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入与碰撞事件 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 等待下一帧、定时器与资源加载，`EndPlay` 时取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新与存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>

## Limitations

- 支持部分 C# 语法和 .NET API，不能直接使用任意 NuGet 包。见[语言支持清单](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- 泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 中不支持 `await`。见[异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- 方法体可热重载。新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。
- Shipping、Android、iOS 和真实多人游戏验收未完成。

<details>
<summary>Experimental</summary>

- 静态字段、静态构造函数及初始化失败后的异常缓存：已通过同步执行测试，仅限[编译器 API](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)，尚未接入普通构建。
- 同步方法、属性访问器中的显式 `throw`：部分支持，需[实验性构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)。
- 同步调用抛出的异常传入异步 Task：已有运行时测试与源码分析支持，[WASM 编译接入](Docs/Phase66/P66.C10_Synchronous_Error_Task_Transfer.md)尚未完成。

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

Roslyn 只参与编译。运行时通过 `ObjectHandle` 访问 UE 对象，不向脚本暴露 `UObject*`。

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
