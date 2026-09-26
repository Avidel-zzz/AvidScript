# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 源码编译为 WebAssembly，由 UE 插件加载执行；运行时不需要 .NET CLR。

> 开发预览。目前以 UE 5.8 / Win64 Editor、Development 为测试环境，仅支持部分 C# 语法。参见 [Limitations](#limitations)。

[Install](#quick-start) · [Usage](#usage) · [Examples](#examples) · [Development](#development) · [Limitations](#limitations)

## Usage

沿 X 轴移动 Actor，每秒 120 个 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector offset = new FVector(120.0f * deltaSeconds, 0.0f, 0.0f);
    UE.Self.AddActorWorldOffset(offset);
}
```

`UE.Self` 指向挂载脚本的 Actor；`avid_on_tick` 是每帧回调的导出名。以上代码可替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 方法。

<a id="快速开始"></a>
<a id="安装"></a>
<a id="installation"></a>

## Quick start

### Requirements

- Unreal Engine 5.8 源码版
- Visual Studio 2022，已安装 UE C++ 开发工具
- PowerShell 7
- .NET SDK [8.0.416](global.json)

### Install

在 UE 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建 Editor target（替换工程名和引擎路径）：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

### Run the sample

1. 打开工程，放置一个 Cube，将 **Mobility** 设为 **Movable**。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，重新执行 **Build And Bind**。

<a id="示例"></a>

## Examples

| Sample | 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | BeginPlay / Tick / EndPlay、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `async / await` 延迟、取消等待 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器之间的函数调用（RPC） |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步与变更回调（RepNotify） |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 声明 Actor、Component、Subsystem |

延迟后缩放 Actor，摘自 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);

UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
```

完整示例包含取消源的创建、`EndPlay` 清理和[构建配置](Samples/CSharp/LatentGameplay/README.md)。

<a id="当前边界"></a>
<a id="已知限制"></a>
<a id="支持范围"></a>

## Limitations

- 不能直接运行任意 .NET 程序或 NuGet 包。C# 语法支持范围见[语言实现计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- `catch`、`finally` 中不能使用 `await`；异步异常处理需[显式启用](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)。取消支持见 [Task 取消文档](Docs/Phase66/P66.C7_Async_Cancellation_Language_Contract.md)。
- Task 局部变量的重赋值、循环内声明尚不能编译为可运行的 WASM。见[实现进度](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md)。
- 修改 C# 中的 `UClass`、`UProperty`、`UFunction` 声明后，需要重新构建并重启 Editor。
- Android、iOS、Shipping 和真实多人游戏尚未验收。

<a id="构建与测试"></a>
<a id="开发"></a>
<a id="build-and-test"></a>
<a id="build--test"></a>

## Development

以下命令在 `Plugins/AvidScript` 下执行，.NET SDK 版本由 [global.json](global.json) 固定。

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<a id="项目结构"></a>
<a id="internals"></a>

### Layout

```text
Source/   UE 插件模块
Tools/    C# 编译器与 WASM 生成工具
Build/    构建与测试脚本
Samples/  示例
Docs/     文档
```

### Architecture

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

Roslyn 负责 C# 分析，Guest IR 是编译器的中间表示。WASM 脚本通过 `ObjectHandle`（对象句柄）访问 UE 对象；Roslyn 和 CLR 不进入 UE Runtime。

<a id="许可"></a>

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
