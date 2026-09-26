# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 为 Unreal Engine 提供 C# 脚本支持。源码编译为 WebAssembly，运行时不依赖 .NET CLR。

**开发中。** 当前测试环境为 UE 5.8 / Win64 Editor、Development。支持的 C# 语法和平台范围见 [Limitations](#limitations)。

[Quick start](#quick-start) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

## Usage

每帧移动 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 沿 X 轴移动，速度 120 cm/s。
    FVector offset = new FVector(120.0f * deltaSeconds, 0.0f, 0.0f);
    UE.Self.AddActorWorldOffset(offset);
}
```

`UE.Self` 是挂载脚本的 Actor，`avid_on_tick` 是 Tick 回调的导出名。这段代码用于替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 方法。

<a id="快速开始"></a>
<a id="安装"></a>
<a id="installation"></a>

## Quick start

<a id="requirements"></a>

依赖：**UE 5.8 源码版**、**Visual Studio 2022**（UE C++ 开发工具）、**PowerShell 7**、**.NET SDK [8.0.416](global.json)**。

<a id="install"></a>

**1. 安装插件** — 在 UE 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

**2. 构建 Editor** — 替换下面的引擎路径、工程文件名和 target 名：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="run-the-sample"></a>

**3. 运行示例**

- 打开工程，放置一个 Cube，将 **Mobility** 设为 **Movable**。
- 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
- 点击 **Play**。默认示例会让 Cube 移动、旋转和缩放。

脚本位于 [Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。修改后重新执行 **Build And Bind**。

<a id="示例"></a>

## Examples

| 用法 | 示例 |
| --- | --- |
| BeginPlay / Tick / EndPlay、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| `async / await` 延迟与取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| 客户端 / 服务器 RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 属性同步、RepNotify 变更回调 | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 更新、存档读写 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

例如，在 `LatentGameplay` 的 `BeginPlay` 中等待 0.25 秒，再缩放 Actor：

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);

UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
```

取消源的创建和 `EndPlay` 清理见[完整脚本](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)，编译步骤见[示例说明](Samples/CSharp/LatentGameplay/README.md)。

<a id="当前边界"></a>
<a id="已知限制"></a>
<a id="支持范围"></a>

## Limitations

| 范围 | 当前限制 |
| --- | --- |
| C# / .NET | 仅支持部分 C# 语法，不能直接运行任意 .NET 程序或 NuGet 包。见[语言支持计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。 |
| 异步 | `catch`、`finally` 中不支持 `await`；异步异常处理需[显式启用](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)。[Task 取消](Docs/Phase66/P66.C7_Async_Cancellation_Language_Contract.md)和[局部变量生命周期](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md)仍在开发。 |
| UE 类型声明 | 修改 `UClass`、`UProperty`、`UFunction` 声明后，需要重新构建并重启 Editor。 |
| 平台与发布 | Android、iOS、Shipping 和真实多人游戏尚未验收。 |

<a id="构建与测试"></a>
<a id="开发"></a>
<a id="build-and-test"></a>
<a id="build--test"></a>

## Development

在 `Plugins/AvidScript` 下执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<a id="项目结构"></a>
<a id="internals"></a>
<a id="layout"></a>

```text
Source/   UE 插件模块
Tools/    C# 编译器与 WASM 生成工具
Build/    构建与测试脚本
Samples/  示例
Docs/     文档
```

<a id="architecture"></a>

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

编译器使用 Roslyn 分析 C#，经 Guest IR（中间表示）生成 WASM。脚本通过 `ObjectHandle` 句柄访问 UE 对象，不持有裸指针。

<a id="许可"></a>

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
