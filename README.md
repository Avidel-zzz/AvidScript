# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，通过生成的绑定调用 UE API，不托管 CLR。

**状态：开发预览。** 当前测试平台为 UE 5.8 / Win64 Editor、Development。

[安装](#安装) · [示例](#示例) · [支持范围](#支持范围) · [开发](#开发) · [License](#license)

```csharp
// 每帧沿 X 轴移动 120 * deltaSeconds；UE.Self 为绑定脚本的 Actor。
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[完整源码 →](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)

<a id="快速开始"></a>

## 安装

依赖：UE **5.8 源码版**、Visual Studio 2022（UE C++ 工具）、PowerShell 7、.NET SDK [**8.0.416**](global.json)。

以下命令使用 `AvidTPSTemplate` 工程，工作目录为 `Plugins/AvidScript`：

```powershell
# 安装 Wasmtime 运行库
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

# 构建 Editor
$ueRoot = 'C:\UnrealEngine' # 改为本机 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开 `AvidTPSTemplate.uproject`。
2. 放置一个 Cube，设为 **Movable** 并选中，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。Cube 开始移动、旋转、放大。

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，再执行同一菜单命令。

## 示例

| 源码 / 说明 | 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入和碰撞回调 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 延时与取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制与 `RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 与存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API、类型转换 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 声明 Actor、Component、Subsystem |

<details>
<summary><b>async / await</b> — 等待 0.25 秒后缩放 Actor</summary>

节选自 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：

```csharp
private static AvidCancellationSource LifetimeCancellation;

[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    LifetimeCancellation = AvidCancellationSource.Create();

    await UKismetSystemLibrary.DelayAsync(0.25f)
        .WithCancellation(LifetimeCancellation.Token);
    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}

[UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
public static void EndPlay()
{
    LifetimeCancellation.Cancel();
    LifetimeCancellation.Release();
}
```

</details>

<a id="当前边界"></a>
<a id="已知限制"></a>

## 支持范围

| 项目 | 当前限制 |
| --- | --- |
| C# / .NET | 支持 C# 子集；不能直接运行任意 .NET 程序或 NuGet 包 |
| `Task<int>` | 仅限同一脚本实例内等待；`catch` 尚不能捕获异步取消，外层 `await` 可能中止脚本 |
| `try` / `await` | 需要[预览开关](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)；`catch`、`finally` 内不支持 `await` |
| C# 声明 UE 类型 | 修改 `UClass`、`UProperty`、`UFunction` 声明后，需重新构建并重启 Editor |
| 平台与发布 | Android、iOS、Shipping、真实多人游戏尚未验收 |

<a id="构建与测试"></a>

## 开发

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 脚本，生成 WASM
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

[语言实现与测试](Docs/Phase66/P66.C_Language_Execution_Plan.md) · [Task](Docs/Phase66/P66.C4_Task_Result_Contract.md) · [异步取消](Docs/Phase66/P66.C7_Async_Cancellation_Language_Contract.md) · [开发约定](AGENTS.md)

<a id="许可"></a>

## License

[MIT](LICENSE) · [Wasmtime](Source/ThirdParty/Wasmtime/README.md) · [WAMR](Source/ThirdParty/WAMR/README.md)

第三方依赖保留各自许可证。Unreal Engine 不包含在本仓库中。
