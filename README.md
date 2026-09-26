# AvidScript

<img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="720">

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。将 C# 编译为 WebAssembly，通过生成的接口调用 UE API，运行时不依赖 CLR。

**开发预览** · UE 5.8 · Win64 Editor / Development · [C# 支持限制](#已知限制)

[快速开始](#快速开始) · [示例](#示例) · [已知限制](#已知限制) · [构建与测试](#构建与测试)

```csharp
// Tick 回调：沿 X 轴移动，每秒 120 UE 单位。
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self：当前脚本绑定的 Actor。
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

节选自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。`deltaSeconds` 单位为秒。

## 快速开始

### 环境

- Unreal Engine 5.8 源码版
- Visual Studio 2022，含 UE C++ 开发工具
- PowerShell 7
- .NET SDK **8.0.416**（[global.json](global.json)）

### 构建插件

以下步骤使用 `AvidTPSTemplate` 工程。在 `Plugins/AvidScript` 目录执行：

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

### 运行

1. 打开 `AvidTPSTemplate.uproject`。
2. 放置一个 Cube，设为 **Movable** 并选中，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。Cube 开始移动、旋转、放大。

修改 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs` 后，用同一菜单命令重新编译并绑定。

## 示例

| 示例 | API / 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入和碰撞回调 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 延时与取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制与 `RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 与存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API、类型转换 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 声明 Actor、Component、Subsystem |

<details>
<summary>async / await：延时后缩放 Actor</summary>

节选自 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：

```csharp
private static AvidCancellationSource LifetimeCancellation;

[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    LifetimeCancellation = AvidCancellationSource.Create();

    // 延时 0.25 秒。
    await UKismetSystemLibrary.DelayAsync(0.25f)
        .WithCancellation(LifetimeCancellation.Token);
    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}

[UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
public static void EndPlay()
{
    // 取消等待并释放取消源。
    LifetimeCancellation.Cancel();
    LifetimeCancellation.Release();
}
```

</details>

<a id="当前边界"></a>

## 已知限制

- 仅支持部分 C# 语法，不能直接运行任意 .NET 程序或 NuGet 包。
- [`Task<int>`](Docs/Phase66/P66.C4_Task_Result_Contract.md) 仅支持同一脚本实例内等待；外层 `await` 未处理的取消仍可能中止脚本执行。
- 在 `try` 中直接 `await` 需要[预览开关](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)；`catch`、`finally` 内尚不支持 `await`。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。
- Android、iOS、Shipping 和真实多人游戏尚未验收。

## 构建与测试

在仓库根目录执行：

```powershell
# 编译 ActorLifecycle 脚本，生成 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

更多验证命令见[语言实现与测试记录](Docs/Phase66/P66.C_Language_Execution_Plan.md)，包括[异步热重载与失败回滚](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md#独立-session-的代码更新与回滚)。

## 项目结构

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

```text
Source/    UE 插件模块
Tools/     C# 编译器、中间表示、WASM 生成工具
Build/     构建与验证脚本
Samples/   示例脚本
Docs/      使用说明、设计与测试记录
```

贡献前请阅读 [AGENTS.md](AGENTS.md)。

## 许可

[MIT](LICENSE)。第三方依赖保留各自许可证，见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) 和 [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
