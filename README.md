# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 代码编译为 WebAssembly，通过 Wasmtime 或 WAMR 在 UE 中运行，不依赖 .NET 运行时。

目前处于实验阶段，主要在 UE 5.8 / Win64 上开发和测试。使用前请查看[已知限制](#known-limitations)。

[安装](#installation) · [使用](#usage) · [示例](#samples) · [已知限制](#known-limitations) · [开发](#development)

<a id="quick-start"></a>

## Installation

需要以下环境：

- Unreal Engine 5.8 源码版
- Visual Studio 2022，安装 UE C++ 工具链
- PowerShell 7
- .NET SDK [8.0.416](global.json)（用于编译脚本）

在 UE C++ 工程目录克隆插件并安装 Wasmtime：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

在 `Plugins/AvidScript` 目录编译 Editor。将 `MyGame` 和 `$ueRoot` 换成自己的工程名与引擎路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

## Usage

### 绑定到现有 Actor

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

脚本位于 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。例如，将其中的 `Tick` 改为沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是绑定的 Actor，移动速度为 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

保存后重新执行 **Build And Bind** 加载修改。

### 定义蓝图可用的类型

用 `[UClass]` 声明 Actor，`[UProperty]` 和 `[UFunction]` 暴露蓝图属性与函数：

```csharp
using AvidScript;

namespace AvidScriptSamples;

[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public async void SetLaunchSpeedNextTick(float speed)
    {
        await AvidContinuations.NextTickAsync();
        LaunchSpeed = speed;
    }
}
```

蓝图可以继承 `Projectile`。调用 `SetLaunchSpeedNextTick(900.0f)`，下一帧的 `LaunchSpeed` 就是 `900`。

新增这类 UE 类型需要重新编译 Editor；只改方法体可以热重载。参见[完整示例与构建说明](Samples/CSharp/ScriptDefinedTypes/README.md)。

<a id="examples"></a>

## Samples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | Actor 移动、生命周期、输入与碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 等待后缩放 Actor，在 `EndPlay` 时取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 调用 Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，用 `RepNotify` 处理客户端更新 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 更新 UI、读写存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>
<a id="limitations"></a>

## Known limitations

支持范围还在补全：

- **C#：**[静态对象字段与静态构造函数](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)已通过同步执行测试，但仍是编译 API 预览，普通构建尚未开放。仅支持部分 .NET API，不能直接使用任意 NuGet 包。
- **异步：**泛型 Task 目前只有 `Task<int>`；不能在 `catch` / `finally` 中使用 `await`。
- **异常：**部分同步方法和属性访问器可使用显式 `throw`，需要[实验性构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)。同步异常还不能向异步方法传播。
- **热重载：**新增 UE 类型、属性、函数，或修改反射签名，需要重新编译并重启 Editor。
- **平台：**当前测试目标是 Win64 Editor / Development。Shipping、Android、iOS 和真实多人游戏验收尚未完成。

[C# 支持范围与测试记录](Docs/Phase66/P66.C_Language_Execution_Plan.md) · [Task 异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)

## Development

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

编译与运行流程：

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

Roslyn 分析 C#，Guest IR 是编译器的中间表示。脚本通过 `ObjectHandle` 访问 UE 对象，不持有裸 `UObject*`。

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
