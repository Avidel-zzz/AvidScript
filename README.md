# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

C# scripting for Unreal Engine. 基于 Roslyn 和 WebAssembly，运行时不依赖 CLR。

**Experimental** · UE 5.8 · Win64 Editor / Development

[Usage](#usage) · [Quick start](#quick-start) · [Samples](#samples) · [Limitations](#limitations) · [Development](#development)

## Usage

[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) — C# Actor、蓝图属性、异步方法。

```csharp
using AvidScript;

namespace AvidScriptSamples;

// 支持蓝图子类
[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    // 蓝图可读写的属性
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    // 蓝图可调用的方法
    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public async void SetLaunchSpeedNextTick(float speed)
    {
        await AvidContinuations.NextTickAsync(); // 等待下一帧
        LaunchSpeed = speed;
    }
}
```

调用 `SetLaunchSpeedNextTick(900)`，下一帧 `LaunchSpeed` 变为 `900`。[构建与热重载](Samples/CSharp/ScriptDefinedTypes/README.md)。

## Quick start

- Unreal Engine 5.8 源码版
- Visual Studio 2022，安装 UE C++ 开发工具
- PowerShell 7
- .NET SDK [8.0.416](global.json)

<a id="installation"></a>

### Build

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor（将 `MyGame` 和引擎路径替换为实际值）：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

### Run

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 可改为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 沿 X 轴移动，速度 120 cm/s
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor。修改后重新执行 **Build And Bind**。

<a id="examples"></a>

## Samples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick` / `EndPlay`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `async` / `await`、延迟、取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端 / 服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 项目 C++ API 绑定 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>

## Limitations

| 范围 | 当前限制 |
| --- | --- |
| C# / .NET | 支持部分语法和类库；不能直接使用任意 NuGet 包。[语言支持](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| `async` / `await` | 带返回值的 Task 仅支持 `Task<int>`；`catch` / `finally` 中不支持 `await` |
| 热重载 | 支持方法体修改；新增属性、修改函数签名需重新编译并重启 Editor |
| 平台与发布 | Android、iOS、Shipping、真实多人游戏验收未完成 |

<details>
<summary>实验性 Task 异常与取消</summary>

通过[构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)启用。
行为和测试记录见 [Task 异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。

</details>

## Development

工作目录：`Plugins/AvidScript`。

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
