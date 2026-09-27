# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，通过 Wasmtime / WAMR 运行，无需在 UE 中托管 CLR。

**Status:** Development preview. [语言与平台限制](#limitations)

[Install](#installation) · [Usage](#usage) · [Examples](#examples) · [Development](#development) · [License](#license)

<a id="installation"></a>
<a id="getting-started"></a>
<a id="quick-start"></a>

## Installation

依赖：

- Unreal Engine 5.8 源码版
- Visual Studio 2022，含 UE C++ 工具链
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

从 UE C++ 工程根目录安装：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor target（替换引擎路径和项目名 `MyGame`）：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

## Usage

1. 打开 Editor，放置一个 Cube，将 Mobility 设为 `Movable` 并选中。
2. 点击 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。示例会移动、旋转和缩放 Cube。

脚本位于 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。将其中的 `Tick` 改为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是绑定脚本的 Actor。沿 X 轴移动，速度 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

停止 Play，重新执行上述构建菜单，再次 Play 即可运行修改后的脚本。

<a id="samples"></a>
<a id="examples"></a>

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入、碰撞 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | `[UClass]`、`[UProperty]`、`[UFunction]`、蓝图继承 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await`、定时器、销毁时取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 项目 C++ API 绑定 |

<details>
<summary>C# 定义 Actor / Blueprint 属性 / 函数</summary>

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

生成并编译后，蓝图可继承 `Projectile`、读写 `LaunchSpeed`、调用 `GetLaunchSpeed()`。
新增类型、属性或函数需要重新编译并重启 Editor。构建步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

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

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## Limitations

- **C# / .NET：** 支持部分语法和 API，不能直接引用任意 NuGet 包。见 [语言支持进度](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **Async：** 泛型 Task 仅支持 `Task<int>`；`catch`、`finally` 内不能使用 `await`。见 [异步异常支持](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- **热重载：** 支持方法体修改。新增 UE 类型、成员或修改反射签名，需要重新编译并重启 Editor。
- **平台：** Shipping、Android、iOS 和真实多人游戏的完整验收尚未完成。

<details>
<summary>实验性编译器功能</summary>

以下功能需要编译器 API 或额外参数，默认构建命令尚未全部支持。各文档包含启用方式与限制：

- [静态字段初始化](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)：`static int Count = InitCount();`
- [同步异常](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)：同步方法与属性访问器中的 `throw`。
- [异步成员赋值](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证)：`target.Value = await GetValueAsync();`，暂不支持接口属性和生成的 UE 类型。
- [取消与求值顺序](Docs/Phase66/P66.C10_PreCancelled_Await_Contract.md)：`DelayAsync(ReadDelay()).WithCancellation(ReadToken())`，参数按从左到右顺序各求值一次，已取消时同步执行取消清理。

</details>

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
