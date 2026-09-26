# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 为 Unreal Engine 提供 C# 脚本支持。脚本编译为 WebAssembly，由 Wasmtime / WAMR 执行，运行时不依赖 CLR。

**Development preview** · UE 5.8 · Win64 · [已知限制](#limitations)

[Quick start](#quick-start) · [Examples](#examples) · [Build & test](#development) · [License](#license)

C# 声明 Actor、属性和函数（[完整示例](Samples/CSharp/ScriptDefinedTypes/README.md)）：

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

生成并编译后，蓝图可继承 `Projectile`、读写 `LaunchSpeed`、调用 `GetLaunchSpeed()`。新增反射成员需要重新编译 Editor。

<a id="installation"></a>
<a id="getting-started"></a>
<a id="quick-start"></a>

## Quick start

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7、[.NET SDK 8.0.416](global.json)。以下命令从 UE C++ 工程根目录开始。

### Install

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译项目的 Editor target；将引擎路径和 `MyGame` 替换为实际值：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

### Run

1. 在 Editor 中放置并选中一个 `Movable` Cube。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

脚本：[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。例如，将 `Tick` 替换为沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self：绑定脚本的 Actor；速度：120 cm/s
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

修改后停止 Play，重新执行构建菜单，再次 Play。

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

<a id="development"></a>

## Build & test

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## Known limitations

| 范围 | 限制 |
| --- | --- |
| [C# / .NET](Docs/Phase66/P66.C_Language_Execution_Plan.md) | 支持部分语法和 API；不能直接引用任意 NuGet 包 |
| [Async](Docs/Phase66/P66.C9_Async_Throw_Routing.md) | 泛型 Task 仅支持 `Task<int>`；`catch`、`finally` 内不支持 `await` |
| 热重载 | 支持修改方法体；新增 UE 类型、成员或修改反射签名，需重新编译并重启 Editor |
| 平台 | Shipping、Android、iOS 和真实多人游戏的完整验证尚未完成 |

<details>
<summary>Experimental compiler features</summary>

需要编译器 API 或额外构建参数，尚未全部接入默认构建命令：

| 语法 | 支持范围 |
| --- | --- |
| `static int Count = InitCount();` | [静态字段初始化与对象生命周期](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md) |
| 同步方法和属性访问器中的 `throw` | [同步异常与构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build) |
| `target.Value = await GetValueAsync();` | [异步成员赋值](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证)；暂不支持接口属性和生成的 UE 类型 |
| `DelayAsync(ReadDelay()).WithCancellation(ReadToken())` | [取消与求值顺序](Docs/Phase66/P66.C10_PreCancelled_Await_Contract.md)；参数从左到右各求值一次，已取消时同步执行取消清理 |

</details>

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
