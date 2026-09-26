# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。脚本编译为 WebAssembly，由 Wasmtime 或 WAMR 执行，不依赖 CLR。

目前主要支持 **UE 5.8 / Win64**，仍在开发中。C# 语法和 .NET API 的支持范围见[限制](#limitations)。

[快速开始](#installation) · [示例](#examples) · [限制](#limitations) · [开发](#development)

脚本通过 `UE.Self` 访问绑定的 Actor。例如，在每帧回调中移动 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 沿 X 轴移动，速度 120 cm/s
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

完整脚本：[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。以上代码可替换其中的 `Tick` 方法。

<a id="quick-start"></a>
<a id="getting-started"></a>
<a id="installation"></a>

## 快速开始

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7、[.NET SDK 8.0.416](global.json)。

### 安装

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor target。替换下面的引擎路径和 `MyGame` 工程名：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

### 运行

1. 打开 Editor，在关卡中放置 Cube，设为 `Movable` 并选中。
2. 选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建完成后点击 **Play**，Cube 会移动、旋转和缩放。

修改脚本后，再次执行该菜单命令即可重新构建。

<a id="samples"></a>
<a id="examples"></a>

## 示例

| 示例 | API / 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await`、定时器、取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | `[UClass]`、`[UProperty]`、`[UFunction]`、蓝图继承 |

<details>
<summary>示例：用 C# 定义 Actor 和蓝图属性</summary>

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

按 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) 生成并编译后，`Projectile` 可作为蓝图父类，`LaunchSpeed` 可在蓝图中读写，`GetLaunchSpeed()` 对应纯函数节点。

</details>

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 限制

| 项目 | 当前限制 |
| --- | --- |
| C# / .NET | 仅支持部分语法和 API，不能直接引用任意 NuGet 包。[支持清单](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| `async` / `await` | 泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`。[详细说明](Docs/Phase66/P66.C9_Async_Throw_Routing.md) |
| 热重载 | 支持修改方法体；新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。 |
| 平台 | Shipping、Android、iOS 和真实多人游戏的完整验证尚未完成。 |

<details>
<summary>实验性编译器功能</summary>

以下功能需要编译器 API 或实验性参数，尚未全部接入默认构建命令：

- [静态字段初始化、静态构造函数](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)：通过编译器 API 使用。
- [同步方法和属性访问器中的 `throw`](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)：部分用法可用，需开启实验性参数。
- [在 `async` 方法中捕获同步异常](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md#windows-专项验证)：包括静态初始化异常，通过编译器 API 使用。
- [`target.Value = await ReadAsync()`](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证)：支持普通 C# 对象的字段和属性；接口属性、生成的 UE 类型尚不支持。

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

编译器使用 Roslyn 分析 C#，经 Guest IR 生成 WASM。运行时通过 `ObjectHandle` 访问 UE 对象，脚本不持有裸 `UObject*`。

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

第三方依赖说明：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
