# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 为 Unreal Engine 提供 C# 脚本支持。C# 编译为 WebAssembly，由 Wasmtime / WAMR 执行；UE 进程不加载 .NET 运行时。

**Experimental.** 当前开发环境为 UE 5.8 / Win64。C#、.NET API 和发布平台的支持范围见 [Known limitations](#known-limitations)。

[Quick start](#quick-start) · [Usage](#usage) · [Samples](#samples) · [Development](#development) · [License](#license)

## Usage

用 C# 定义 Actor，向蓝图暴露属性和函数：

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

蓝图可继承 `Projectile`、读写 `LaunchSpeed`。调用 `SetLaunchSpeedNextTick(900.0f)` 后，属性在下一帧更新为 `900`。

[完整源码](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) · [构建与热重载](Samples/CSharp/ScriptDefinedTypes/README.md)

<a id="installation"></a>

## Quick start

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具链）、PowerShell 7、.NET SDK [8.0.416](global.json)。

### 1. Install

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

### 2. Build

在 `Plugins/AvidScript` 下执行，将 `MyGame` 和 `$ueRoot` 改为自己的工程名与引擎路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

### 3. Run

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

将 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 替换为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self：绑定的 Actor。沿 X 轴移动 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

保存后重新执行 **Build And Bind** 加载修改。

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

Win64 Editor / Development 是当前测试目标。Shipping、Android、iOS 和真实多人游戏验收尚未完成。

| 项目 | 当前限制 |
| --- | --- |
| C# | [静态对象字段与静态构造函数](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)尚未接通 C# → WASM 编译执行。 |
| .NET | 仅支持部分 API，不能直接使用任意 NuGet 包。 |
| `async` / `await` | 泛型 Task 仅支持 `Task<int>`；不支持在 `catch` / `finally` 中 `await`。 |
| 异常 | 部分同步方法和属性访问器支持显式 `throw`，需启用[实验性构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)。同步异常尚不能向异步方法传播。 |
| 热重载 | 支持方法体修改。新增 UE 类型、属性、函数，或修改反射签名，需要重新编译并重启 Editor。 |

[C# 支持范围与测试记录](Docs/Phase66/P66.C_Language_Execution_Plan.md) · [Task 异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)

## Development

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

### Architecture

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

构建工具通过 Roslyn 分析 C#，经 Guest IR（中间表示）生成 WASM。运行时用 `ObjectHandle` 标识和访问 UE 对象，脚本不持有裸 `UObject*`。

```text
Source/    UE 插件模块
Tools/     C# 编译器与代码生成工具
Build/     构建与测试脚本
Samples/   脚本示例
Docs/      设计文档与测试记录
```

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
