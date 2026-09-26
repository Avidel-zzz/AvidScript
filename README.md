# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 将 C# 脚本编译为 WebAssembly，在 Unreal Engine 中运行。通过生成的 C# 绑定调用 UE API，运行时不依赖 CLR。

Development preview · UE 5.8 · Win64 Editor / Development

[Quick start](#quick-start) · [Examples](#examples) · [Limitations](#limitations) · [Development](#development)

```csharp
// Tick：沿 X 轴移动，每秒 120 UE 单位。
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor。完整示例：[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

<a id="快速开始"></a>
<a id="安装"></a>

## Quick start

需要 UE **5.8 源码版**、Visual Studio 2022（UE C++ 工具）、PowerShell 7 和 .NET SDK [**8.0.416**](global.json)。

以下使用 `AvidTPSTemplate` 工程。插件放在 `Plugins/AvidScript`，命令从插件目录执行：

```powershell
# 安装运行库
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

# 构建 Editor
$ueRoot = 'C:\UnrealEngine' # 改为本机 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开 `AvidTPSTemplate.uproject`。
2. 放置 Cube，将 Mobility 设为 **Movable**，保持选中。
3. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
4. 点击 **Play**，Cube 会移动、旋转、放大。

编辑上面的 `ActorLifecycleScript.cs`，再次执行菜单命令即可重新编译并绑定。

<a id="示例"></a>

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入和碰撞回调 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `DelayAsync`、`WithCancellation` |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制与 `RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 与存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | C++ API 绑定、类型转换 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 声明 Actor、Component、Subsystem |

例如，在 `BeginPlay` 中等待 0.25 秒，再缩放 Actor（[函数体节选](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)）：

```csharp
LifetimeCancellation = AvidCancellationSource.Create();

await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
```

完整示例在 `EndPlay` 中调用 `Cancel()` 和 `Release()`，取消等待并释放取消源。

<a id="当前边界"></a>
<a id="已知限制"></a>
<a id="支持范围"></a>

## Limitations

- 编译器支持 C# 子集，不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 只能在同一脚本实例内等待。`catch` 尚不能捕获[异步取消](Docs/Phase66/P66.C7_Async_Cancellation_Language_Contract.md)，外层 `await` 可能中止脚本。
- `try` 中使用 `await` 需要[预览开关](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)；`catch`、`finally` 中暂不支持 `await`。
- 修改 C# 声明的 `UClass`、`UProperty`、`UFunction` 后，需要重新构建并重启 Editor。
- Android、iOS、Shipping 和真实多人游戏尚未验收。

<a id="构建与测试"></a>
<a id="开发"></a>

## Development

从插件目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<a id="项目结构"></a>

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

```text
Source/    UE 插件模块
Tools/     C# 编译器、中间表示、WASM 生成工具
Build/     构建与验证脚本
Samples/   示例脚本
Docs/      使用说明、设计与测试记录
```

编译器支持范围与测试记录见[语言实现文档](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

<a id="许可"></a>

## License

[MIT](LICENSE)。[Wasmtime](Source/ThirdParty/Wasmtime/README.md) 和 [WAMR](Source/ThirdParty/WAMR/README.md) 使用各自许可证。Unreal Engine 不包含在本仓库中。
