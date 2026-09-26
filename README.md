# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。将 C# 编译为 WebAssembly，在 UE 中通过 Wasmtime / WAMR 执行，无需 CLR。

**开发中 · UE 5.8 · Win64**。尚未完整支持 C# / .NET，使用前请查看[已知限制](#limitations)。

[安装](#installation) · [用法](#usage) · [示例](#examples) · [已知限制](#limitations) · [开发](#development)

```csharp
// 每帧沿 X 轴移动 120 cm/s
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor，`avid_on_tick` 是每帧回调。此片段可替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 方法。

<a id="quick-start"></a>
<a id="getting-started"></a>
<a id="installation"></a>

## 安装

- Unreal Engine 5.8 源码版
- Visual Studio 2022，含 UE C++ 工具链
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

将 `MyGame` 换成工程名，修改引擎路径，然后编译：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

## 用法

1. 打开 Editor，在关卡中放置 Cube，设为 `Movable` 并选中。
2. 选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建完成后点击 **Play**，Cube 会移动、旋转和缩放。

修改 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs` 后，通过同一菜单重新构建。

<a id="samples"></a>
<a id="examples"></a>

## 示例

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick`、Actor 移动、输入与碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await`、定时器、任务取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步与 `RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem，蓝图继承 |

<details>
<summary>C# 声明 UE 类型：Projectile</summary>

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

生成并编译后，蓝图可继承 `Projectile`、读写 `LaunchSpeed`，通过纯函数节点调用 `GetLaunchSpeed()`。构建说明见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 已知限制

- **C# / .NET**：仅支持部分语法和 API，不能直接引用任意 NuGet 包。见[语言支持清单](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **`async` / `await`**：泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 中不能 `await`。见[异步支持范围](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- **热重载**：可修改方法体；新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。
- **平台**：Shipping、Android、iOS 和真实多人游戏验收尚未完成。

<details>
<summary>实验性功能（默认构建流程未全部接入）</summary>

- [静态字段初始化、静态构造函数](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)：通过编译器 API 使用，普通构建脚本未接入。
- [同步方法、属性访问器中的 `throw`](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)：仅支持部分用法，需开启实验性参数。
- [同步异常传入 `async` 方法](Docs/Phase66/P66.C10_Synchronous_Error_Task_Transfer.md#c-源码到-vm-的执行验证)：尚不支持与静态初始化组合。
- [`target.Value = await ReadAsync()`](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证)：支持普通 C# 对象的字段、属性；接口、静态初始化组合及生成 UE 类型尚未完成。

</details>

<a id="development"></a>

## 开发

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

编译器用 Roslyn 分析 C#，通过 Guest IR（中间表示）生成 WASM；运行时通过 `ObjectHandle`（对象句柄）访问 UE 对象。

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
