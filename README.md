# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。C# 源码编译为 WebAssembly，在 UE 中运行，无需 CLR。

> 开发预览，当前以 UE 5.8 / Win64 为主。尚不支持完整 C# / .NET，详见[限制](#limitations)。

[示例](#usage) · [构建](#installation) · [运行](#quick-start) · [Samples](#examples) · [限制](#limitations) · [开发](#development)

## Usage

声明一个 Actor，向蓝图暴露属性和方法：

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
        await AvidContinuations.NextTickAsync(); // 等待下一帧
        LaunchSpeed = speed;
    }
}
```

蓝图调用 `SetLaunchSpeedNextTick(900)` 后，`LaunchSpeed` 在下一帧更新为 `900`。

[完整示例](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) · [生成类型与热重载](Samples/CSharp/ScriptDefinedTypes/README.md)

## Installation

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具）、PowerShell 7、.NET SDK [8.0.416](global.json)。

从 UE C++ 工程根目录安装：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor。替换下面的引擎路径、工程名和 Target：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="quick-start"></a>

### Run the sample

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

编辑 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick`，然后重新执行 **Build And Bind**：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 沿 X 轴移动，速度 120 cm/s
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指绑定了脚本的 Actor。

## Examples

| 示例 | 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick` / `EndPlay`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 延迟、取消令牌 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端 / 服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、同步后的回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 项目 C++ API 绑定 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>

## Limitations

- 仅支持部分 C# 语法和类库，不能直接运行任意 NuGet 包。见[语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- 带返回值的 Task 仅支持 `Task<int>`；`catch` / `finally` 中不能 `await`。[Task 异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)仍属实验功能，需通过[构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)开启。
- 热重载支持方法体修改。新增属性、修改函数签名等 UE 类型结构变更，需要重新编译并重启 Editor。
- Android、iOS、Shipping 和真实多人游戏验收尚未完成。

## Development

在 `Plugins/AvidScript` 下执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

[Source/](Source/) — UE 插件 · [Tools/](Tools/) — 编译器 · [Build/](Build/) — 构建脚本 · [Docs/](Docs/) — 设计与实现文档

## License

[MIT](LICENSE)。

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
