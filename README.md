# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。使用 Roslyn 将 C# 编译为 WebAssembly，UE 运行时不加载 CLR。

> Development preview · UE 5.8 · Win64 Editor / Development。C# 与平台限制见 [Limitations](#limitations)。

[Usage](#usage) · [Installation](#installation) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

## Usage

Actor 的 Tick 回调：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // X 轴移动，120 cm/s
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向挂载脚本的 Actor；`avid_on_tick` 导出为 UE Tick 回调。
上面的代码可替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`。

<a id="快速开始"></a>
<a id="安装"></a>
<a id="quick-start"></a>

## Installation

<a id="requirements"></a>

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工具）、PowerShell 7、.NET SDK [8.0.416](global.json)。

<a id="install"></a>

在 UE 工程根目录安装：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建 Editor。按项目修改引擎路径、`.uproject` 和 Editor target：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="run-the-sample"></a>

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

修改 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs` 后，重新执行 **Build And Bind**。

<a id="示例"></a>

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick` / `EndPlay`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `async` / `await`、延迟、取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端 / 服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI、存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 声明 Actor、Component、Subsystem |

`LatentGameplayScript.BeginPlay` 片段：

```csharp
// 等待 0.25 秒后缩放 Actor；取消时结束等待
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);

UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
```

[完整脚本](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)包含取消源初始化和 `EndPlay` 清理。

<a id="当前边界"></a>
<a id="已知限制"></a>
<a id="支持范围"></a>

## Limitations

| 范围 | 当前限制 |
| --- | --- |
| C# / .NET | 仅支持部分 C# 语法，不能直接运行任意 .NET 程序或 NuGet 包。见[语言支持计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。 |
| 异步 | `catch`、`finally` 中不支持 `await`；异步异常处理需[显式启用](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)。Task 值限于 `Task<int>`，已支持[局部变量重赋值和循环作用域](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md)。 |
| UE 类型声明 | 修改 `UClass`、`UProperty`、`UFunction` 声明后，需要重新构建并重启 Editor。 |
| 平台与发布 | Android、iOS、Shipping 和真实多人游戏尚未验收。 |

<a id="构建与测试"></a>
<a id="开发"></a>
<a id="build-and-test"></a>
<a id="build--test"></a>

## Development

工作目录：`Plugins/AvidScript`。

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<a id="项目结构"></a>
<a id="internals"></a>
<a id="layout"></a>

<details>
<summary>Repository layout</summary>

```text
Source/   UE 插件模块
Tools/    C# 编译器与 WASM 生成工具
Build/    构建与测试脚本
Samples/  示例
Docs/     文档
```

</details>

<a id="architecture"></a>

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

编译链：`C# → Roslyn → Guest IR → WASM`。Guest IR 是编译器的中间表示；脚本通过 `ObjectHandle` 句柄访问 UE 对象。

<a id="许可"></a>

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
