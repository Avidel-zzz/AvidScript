# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

用 C# 编写 Unreal Engine 游戏逻辑。通过 Roslyn 编译为 WebAssembly，由 UE 插件执行，无需在 UE 进程中运行 CLR。

**状态：Experimental。** 当前开发和测试环境为 UE 5.8 / Win64 Editor、Development。仅支持部分 C# 和 .NET API，详见 [Known limitations](#known-limitations)。

[Quick start](#quick-start) · [Examples](#samples) · [Known limitations](#known-limitations) · [Build & test](#development)

## Example

下面是 [Projectile](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 的简化示例：

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

蓝图可继承 `Projectile`，读写 `LaunchSpeed`，或调用 `GetLaunchSpeed()`。[构建说明](Samples/CSharp/ScriptDefinedTypes/README.md)

<a id="installation"></a>

## Quick start

### Requirements

- Unreal Engine 5.8 源码版
- Visual Studio 2022 + UE C++ 开发工具
- PowerShell 7
- .NET SDK [8.0.416](global.json)

### 1. 安装插件

在已有 UE C++ 工程的根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

### 2. 编译 Editor

以下命令在 `Plugins/AvidScript` 下执行。将 `MyGame` 替换为工程名，`$ueRoot` 替换为引擎目录：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

### 3. 运行示例

`ActorLifecycle` 示例给已有 Actor 绑定 C# 脚本：

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`，可将行为改为沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是当前绑定的 Actor，移动速度为 120 cm/s。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

保存后重新执行 **Build And Bind**。方法体修改支持热重载；新增 UE 属性或修改函数签名需要重新编译 Editor。

<a id="examples"></a>

## Samples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick` / `EndPlay`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `async` / `await`、定时等待、取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器互相调用方法（RPC） |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，在客户端收到更新时执行 `RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>
<a id="limitations"></a>

## Known limitations

- **C# / .NET**：支持普通类的实例字段初始化和构造函数链；暂不支持静态对象字段，也不能直接使用任意 NuGet 包。完整语法支持仍在开发，见[语言支持记录](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **`async` / `await`**：带返回值的 Task 仅支持 `Task<int>`；`catch` / `finally` 中不支持 `await`。
- **热重载**：新增属性或修改函数签名后，需要重新编译并重启 Editor。
- **发布与平台**：Shipping、Android、iOS 和真实多人游戏验收尚未完成。

<details>
<summary>实验性异常处理</summary>

需显式启用[实验性构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)。目前支持部分同步方法、getter/setter 中的显式 `throw`；尚不支持将同步异常传播到异步方法。

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
