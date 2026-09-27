# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，由 Wasmtime 或 WAMR 执行；UE 运行时不需要 CLR。

**开发中。** 当前主要支持 UE 5.8 源码版 / Win64，C# 语法和 .NET API 支持范围见[已知限制](#limitations)。

[安装](#installation) · [运行示例](#quick-start) · [示例目录](#examples) · [开发](#development) · [已知限制](#limitations)

## 用法

每帧移动 Actor，速度为 120 cm/s：

```csharp
// ActorLifecycleScript.cs：替换 Tick 方法
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是绑定当前脚本的 Actor。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`avid_on_tick` 是插件调用的每帧入口。完整文件：[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

<details>
<summary>声明 UE 类型、属性和函数</summary>

`[UClass]` 声明可被蓝图继承的类型；`[UProperty]` 和 `[UFunction]` 将成员暴露给 UE：

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

生成类型并编译 Editor 后，蓝图可以读写 `LaunchSpeed`、调用 `GetLaunchSpeed()`。新增或修改这些声明需要重新编译并重启 Editor。源码与构建说明：[ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="installation"></a>

## 安装

依赖：Unreal Engine 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7、[.NET SDK 8.0.416](global.json)。

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor。将 `$ueRoot`、`MyGame.uproject` 和 `MyGameEditor` 替换为实际引擎路径、工程文件和 Editor target：

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

## 运行示例

1. 用上面的移动代码替换 `ActorLifecycleScript.cs` 中的 `Tick` 方法。
2. 打开 Editor，在关卡中放置一个 Cube，将 **Mobility** 设为 **Movable**。
3. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
4. 点击 **Play**。Cube 沿 X 轴移动。

这个示例的编辑流程：停止 Play → 修改源码 → Build And Bind → Play。

<a id="samples"></a>
<a id="examples"></a>

## 示例目录

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick`、输入、碰撞 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | `[UClass]`、`[UProperty]`、`[UFunction]`、蓝图继承 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 等待定时器、异步加载资源 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |

<a id="development"></a>

## 开发

以下命令在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

`Tools/` 包含 C# 编译器和测试，`Source/` 包含 UE 插件模块。

[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md) · [迭代计划](Docs/Architecture/AvidScript_Iteration_Roadmap.md) · [语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 已知限制

- **C# / .NET**：支持部分语法和 API，不能直接使用任意 NuGet 包；部分功能需要单独启用。
- **异步**：泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`。详见[异步异常](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- **取消**：暂不能从 C# 读取 [`OperationCanceledException.CancellationToken`](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。
- **热重载**：支持方法体更新。新增 UE 类型、属性、函数或修改签名后，需要重新编译并重启 Editor。
- **平台**：主要测试 UE 5.8 / Win64。Shipping、Android、iOS 和完整多人游戏流程仍待验收。

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
