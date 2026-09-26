# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。脚本编译为 WebAssembly，由 Wasmtime / WAMR 运行，UE 进程不加载 CLR。

开发中，目前使用 UE 5.8 / Win64 测试。只支持部分 C# 语法和 .NET API，详见[已知限制](#known-limitations)。

[Quick start](#quick-start) · [Examples](#examples) · [Known limitations](#known-limitations) · [Development](#development)

## Example

以下代码节选自 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)：

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

构建后，蓝图可以继承 `Projectile`，读写 `LaunchSpeed`，调用 `GetLaunchSpeed()`。属性默认值为 `1200`。
生成类型的构建要求见[示例说明](Samples/CSharp/ScriptDefinedTypes/README.md)。

<a id="installation"></a>

## Quick start

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。

**1. 安装插件** — 在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

**2. 编译 Editor** — 在插件目录执行，替换工程名和引擎路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

**3. 运行示例** — 先运行仓库自带的 [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)：

- 打开 Editor，在关卡放置 Cube，设置 `Mobility = Movable`。
- 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
- 点击 **Play**，Cube 会移动、旋转和缩放。

将该示例的 `Tick` 替换为下面的代码，保存后再次执行 **Build And Bind**。这段 `Tick` 会让 Actor 沿 X 轴以 120 cm/s 移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是当前绑定的 Actor。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

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
<a id="limitations"></a>
<a id="status"></a>

## Known limitations

- **C# / .NET**：不能直接使用任意 NuGet 包。语言与标准库支持范围见[支持清单](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **`async` / `await`**：泛型 Task 仅支持 `Task<int>`；不支持在 `catch` / `finally` 中 `await`。异常和取消行为见 [Task 文档](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- **热重载**：支持修改方法体。新增 UE 类型、属性、函数或修改反射签名后，需要重新编译并重启 Editor。
- **平台**：当前为 Win64 Editor / Development。Shipping、Android、iOS 和真实多人游戏验收未完成。

<details>
<summary>实验性编译器功能</summary>

- 静态字段、静态构造和初始化失败后的异常缓存已通过同步执行测试。目前只通过[编译器 API](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)使用，尚未接入普通构建。
- 部分同步方法和属性访问器支持显式 `throw`，需要[实验性构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)。同步异常尚不能传播到异步方法。

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

编译时由 Roslyn 分析 C#，经 Guest IR（中间表示）生成 WASM。运行时通过 `ObjectHandle`（对象句柄）访问 UE 对象。

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
