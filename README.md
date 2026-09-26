# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。支持 Actor 生命周期、UE API 调用和蓝图交互；脚本编译为 WebAssembly，运行时不托管 CLR。

> **开发预览** · UE 5.8 · Win64 Editor / Development。支持 C# 子集，[兼容范围](#limitations)。

[安装](#installation) · [用法](#usage) · [示例](#examples) · [已知限制](#limitations) · [开发](#development)

## Usage

### Actor Tick

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 可替换为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 为脚本所在的 Actor。每帧沿 X 轴移动，速度为 120 cm/s。

### C# → Blueprint

声明 Actor，向蓝图暴露属性和异步方法。节选自 [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)：

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

蓝图调用 `SetLaunchSpeedNextTick(900)` 后，`LaunchSpeed` 在下一帧变为 `900`。类型生成、构建和热重载说明见[示例文档](Samples/CSharp/ScriptDefinedTypes/README.md)。

<a id="quick-start"></a>

## Installation

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具）、PowerShell 7、.NET SDK [8.0.416](global.json)。

### 1. 安装插件

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

### 2. 构建 Editor

替换引擎路径、`MyGame.uproject` 和 `MyGameEditor`：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

### 3. 运行示例

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

修改 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs` 后，重新执行 **Build And Bind**。

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick` / `EndPlay`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 延迟、取消令牌 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端 / 服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 项目 C++ API 绑定 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>

## Limitations

| 范围 | 限制 |
| --- | --- |
| C# / .NET | 不兼容完整 .NET 类库，不能直接运行任意 NuGet 包。[语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| `async` / `await` | Task 值限于 `Task<int>`；`catch`、`finally` 中不能使用 `await`。[异步支持范围与构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build) |
| UE 类型声明 | 方法体支持热重载；修改类型、属性、函数签名或反射标记需重新构建并重启 Editor。 |
| 平台与发布 | Android、iOS、Shipping、真实多人游戏验收待完成。 |

Task 异常与取消处理需显式启用。已支持[取消时抛出新异常](Docs/Phase66/P66.C9_Async_Throw_Routing.md)；生成 UE 类型、对象销毁和热重载中的该组合仍待验证。

## Development

工作目录：`Plugins/AvidScript`。

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

```text
Source/   UE 插件模块
Tools/    C# 编译器、WASM 生成工具
Build/    构建与测试脚本
Samples/  示例脚本
Docs/     设计文档与测试记录
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

Roslyn 负责 C# 语法与语义分析，Guest IR 是编译器的中间表示。UE 加载生成的 WASM，脚本通过 `ObjectHandle` 句柄访问引擎对象。

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
