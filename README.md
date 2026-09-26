# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

用 C# 编写 Unreal Engine 游戏逻辑。脚本编译为 WebAssembly，由 UE 插件加载执行。

**开发预览** — UE 5.8 / Win64 Editor、Development。C# 与平台支持情况见[已知限制](#limitations)。

[安装](#installation) · [代码示例](#usage) · [更多示例](#examples) · [已知限制](#limitations) · [开发](#development)

## Usage

C# 声明 Actor，蓝图读写属性、调用方法：

```csharp
using AvidScript;

namespace AvidScriptSamples;

[UClass(Blueprintable = true, BlueprintType = true)] // 可创建蓝图子类
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")] // 蓝图可读写
    public float LaunchSpeed { get; set; } = 1200.0f;

    [UFunction(BlueprintCallable = true, Category = "Projectile")] // 蓝图可调用
    public async void SetLaunchSpeedNextTick(float speed)
    {
        await AvidContinuations.NextTickAsync(); // 等待下一帧
        LaunchSpeed = speed;
    }
}
```

调用 `SetLaunchSpeedNextTick(900)`，下一帧 `LaunchSpeed` 从 `1200` 变为 `900`。
完整源码：[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) · [构建与热重载说明](Samples/CSharp/ScriptDefinedTypes/README.md)

<a id="quick-start"></a>

## Installation

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工具）、PowerShell 7 和 .NET SDK [8.0.416](global.json)。

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建 Editor，将引擎路径、`MyGame.uproject` 和 `MyGameEditor` 换成自己的：

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

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，重新执行 **Build And Bind**。例如，将 `Tick` 改为下面的代码，Cube 就会以 120 cm/s 沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
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

- **C# / .NET**：运行时不托管 CLR，支持的语法和类库有限，不能直接运行任意 NuGet 包。详见[语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **async / await**：带返回值的 Task 目前仅支持 `Task<int>`；`catch`、`finally` 中不支持 `await`。
- **热重载**：支持修改方法体。新增属性、修改函数签名等 UE 类型结构变更，需要重新构建并重启 Editor。
- **平台**：Android、iOS、Shipping 和真实多人游戏验收尚未完成。

<details>
<summary>Task 异常与取消：实验性支持</summary>

需要显式启用[构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)。
[取消时抛出新异常](Docs/Phase66/P66.C9_Async_Throw_Routing.md)已通过编译器、WASM 后端及 Win64 Editor 生成 Actor 的销毁与热重载测试；真实 Play 和打包运行仍待验收。

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

代码目录：[UE 插件](Source/) · [编译器](Tools/) · [构建脚本](Build/) · [设计文档](Docs/)

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
