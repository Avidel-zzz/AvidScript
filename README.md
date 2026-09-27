# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 为 Unreal Engine 提供 C# 脚本支持。脚本编译为 `.wasm`，由 Wasmtime / WAMR 执行，UE 进程不加载 CLR。

支持 Actor 逻辑、蓝图属性与函数、异步调用、RPC 和属性复制。各功能的代码见[示例](#examples)。

> 开发中。当前主要测试 UE 5.8 / Win64；C#、.NET 和平台支持范围见[已知限制](#limitations)。

[安装](#installation) · [用法](#usage) · [示例](#examples) · [构建与测试](#development) · [许可证](#license)

<a id="usage"></a>

## 用法

在 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中，将 `Tick` 替换为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor。上面的代码让它以 120 cm/s 沿 X 轴移动。

安装插件后，在 Editor 中运行：

1. 放置一个 Cube，将 **Mobility** 设为 **Movable** 并选中。
2. 点击 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。

修改脚本后，停止 Play，重新执行构建菜单，再次 Play。

<a id="installation"></a>
<a id="getting-started"></a>
<a id="quick-start"></a>

## 安装

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor。将引擎路径和 `MyGame` 替换为自己的配置：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="samples"></a>
<a id="examples"></a>

## 示例

| 代码 | 用途 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 在 `BeginPlay`、`Tick`、输入和碰撞回调中控制 Actor |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 用 C# 声明 Actor、属性和函数，供蓝图继承或调用 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 等待定时器、加载资源，Actor 销毁时取消等待 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 在客户端和服务器之间调用 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 复制属性，并在客户端通过 `RepNotify` 响应变化 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 更新 UI、保存和读取游戏数据 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目中的 C++ API |

<details>
<summary>代码：声明一个蓝图可继承的 Actor</summary>

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

蓝图可以继承 `Projectile`、读写 `LaunchSpeed`、调用 `GetLaunchSpeed()`。此示例需要生成 UE 类型并编译 Editor，步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="development"></a>

## 构建与测试

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<details>
<summary>编译流程</summary>

C# 源码由 Roslyn 分析后，转换为插件的中间表示（Guest IR），再生成 `.wasm`。UE 加载该文件，脚本通过受检查的对象句柄访问 Actor 等 UE 对象。

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

</details>

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 已知限制

- C# / .NET 仅支持部分语法和 API，不能直接使用任意 NuGet 包。见[语言支持进度](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- 泛型 Task 目前仅支持 `Task<int>`；`catch`、`finally` 内不能使用 `await`。见[异步异常支持](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- 热重载支持方法体修改。新增 UE 类型、属性、函数或修改其签名，需要重新编译并重启 Editor。
- Shipping、Android、iOS 和真实多人游戏的完整验收尚未完成。

<details>
<summary>尚未纳入默认构建的编译器功能</summary>

下列写法需要单独启用，使用前请查看对应文档：

| 写法 | 文档 |
| --- | --- |
| `static int Count = InitCount();` | [静态字段初始化](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md) |
| 同步方法或属性访问器中的 `throw` | [同步异常](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build) |
| `target.Value = await GetValueAsync();` | [异步赋值](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证)；暂不支持接口属性和生成的 UE 类型 |
| `DelayAsync(ReadDelay()).WithCancellation(ReadToken())` | [取消处理与参数求值顺序](Docs/Phase66/P66.C10_PreCancelled_Await_Contract.md) |

取消来源已在运行时保存并随 Task 传播；C# 暂不能读取 `OperationCanceledException.CancellationToken`。见[实现进度](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。

</details>

<a id="license"></a>

## 许可证

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
