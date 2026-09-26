# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

C# scripting for Unreal Engine.

C# 代码通过 Roslyn 编译为 WebAssembly，由 UE 插件加载执行。运行时不托管 CLR。

> **Experimental** — 当前主要支持 UE 5.8 / Win64 Editor、Development。C# 语法和 .NET API 尚未完整支持，见 [Limitations](#limitations)。

[Example](#example) · [Installation](#installation) · [Usage](#usage) · [Samples](#samples) · [Development](#development)

## Example

用 C# 定义 Actor，向蓝图暴露属性和方法：

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

[完整源码](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) · [构建与热重载](Samples/CSharp/ScriptDefinedTypes/README.md)

<a id="quick-start"></a>

## Installation

### Requirements

- Unreal Engine 5.8 源码版
- Visual Studio 2022，安装 UE C++ 开发工具
- PowerShell 7
- .NET SDK [8.0.416](global.json)

### Build

在已有 UE C++ 工程的根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

将 `MyGame` 替换为工程名，`$ueRoot` 改为引擎目录，然后编译 Editor：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

## Usage

给关卡中已有的 Actor 绑定脚本：

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

编辑 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)，例如让 Actor 沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self：当前绑定的 Actor；速度：120 cm/s
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

修改后重新执行 **Build And Bind**。

<a id="examples"></a>

## Samples

| 示例 | API / 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick` / `EndPlay`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `async` / `await`、定时等待、取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | RPC：客户端与服务器之间调用方法 |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制、`RepNotify`：客户端收到属性更新后的回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>

## Limitations

| 项目 | 限制 |
| --- | --- |
| C# / .NET | 仅支持部分语法和类库，不能直接使用任意 NuGet 包。[支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| `async` / `await` | 带返回值的 Task 仅支持 `Task<int>`；`catch` / `finally` 中不支持 `await` |
| 热重载 | 支持方法体修改；新增属性、修改函数签名需重新编译并重启 Editor |
| 平台 | Android、iOS 验收未完成 |
| 发布 | Shipping、真实多人游戏验收未完成 |

<details>
<summary>实验性异常处理</summary>

需要显式启用[构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)。支持部分同步方法、getter/setter 中的显式 `throw`；同步异常传播到异步方法尚不支持。

- [同步异常测试用例](Tools/AvidScript.CSharpGuest.Tests/CSharpGuestMemberErrorTests.cs)
- [Task 异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)

</details>

## Development

以下命令在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

```text
Source/    UE 插件模块
Tools/     C# 编译器与代码生成工具
Build/     构建与测试脚本
Samples/   示例项目
Docs/      设计文档与测试记录
```

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
