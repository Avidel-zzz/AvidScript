# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# → WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor / Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

AvidScript 将 C# 编译为 WebAssembly，在 Unreal Engine 中运行。通过生成的 C# 绑定访问 UE API，支持 Wasmtime / WAMR，运行时不依赖 CLR。

**开发预览版** · UE 5.8 源码版 · Win64 Editor / Development

[快速开始](#quick-start) · [示例](#examples) · [限制](#limitations) · [构建与测试](#development) · [文档](#docs)

<a id="用法"></a>
<a id="usage"></a>

## 代码示例

在 `Tick` 中移动 Actor，`UE.Self` 指向绑定脚本的 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 沿 X 轴移动，120 cm/s
    UE.Self.AddActorWorldOffset(
        new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

完整源码：[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

<a id="getting-started"></a>
<a id="quick-start"></a>
<a id="installation"></a>

## 快速开始

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7、[.NET SDK 8.0.416](global.json)。

### 安装

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor。将 `MyGame` 和引擎路径替换为实际值：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="运行示例"></a>

### 运行 ActorLifecycle

1. 打开 Editor，选中关卡里的 Cube，将 **Mobility** 设为 **Movable**。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建成功后点击 **Play**。

默认示例会移动、旋转并缩放 Cube。修改脚本后，停止 Play，重新执行 Build And Bind，再点击 Play。

<a id="示例目录"></a>
<a id="samples"></a>
<a id="examples"></a>

## 示例

| 示例 | API / 功能 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick`、输入、碰撞、资源加载 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | `[UClass]` / `[UProperty]` / `[UFunction]`、蓝图继承 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await DelayAsync(0.25f)`、取消等待 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / NetMulticast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 生成项目 C++ API 的 C# 绑定 |

<details>
<summary>C# 声明 UE 类型：Actor、属性、函数</summary>

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

蓝图可继承 `Projectile`，读写 `LaunchSpeed`，调用 `GetLaunchSpeed()`。类型声明变更需要重新生成代码、编译并重启 Editor，见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="已知限制"></a>
<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 限制

默认构建的支持范围：

- **C# / .NET**：部分语法和标准库 API；不支持任意 NuGet 包。
- **异步**：泛型 Task 仅支持 `Task<int>`；不支持在 `catch` / `finally` 内 `await`。
- **异常**：`async` 中尚不支持读取 `catch (Exception error)` 的 `error` 或 `OperationCanceledException.CancellationToken`。
- **热重载**：支持方法体变更。新增 UE 类型、属性、函数或修改签名，需重新编译并重启 Editor。
- **发布**：Shipping、Android、iOS 和完整多人游戏流程尚未完成验收。

实验性 `CancellationToken` 编译支持及 UE 接入进度见[取消与异常支持](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。

<a id="开发"></a>
<a id="development"></a>

## 构建与测试

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

```text
Source/    UE 插件模块
Tools/     C# 编译器、代码生成器与测试
Build/     构建脚本
Samples/   示例源码
Docs/      文档
```

<a id="docs"></a>

## 文档

![C# 编译为 WASM，再由 UE 运行时加载并访问 UObject](Docs/Assets/README/pipeline.svg)

- [模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)
- [C# 支持范围与实现进度](Docs/Phase66/P66.C_Language_Execution_Plan.md)
- [开发路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
