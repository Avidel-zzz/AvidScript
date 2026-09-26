# AvidScript

<img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="720">

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

在 Unreal Engine 中用 C# 编写游戏逻辑。脚本编译为 WebAssembly，通过生成的 C# 接口调用 UE API，运行时不加载 CLR。

> 开发中。当前测试环境为 UE 5.8 / Win64 Editor、Development；C# 支持范围见[已知限制](#已知限制)。

[快速开始](#快速开始) · [示例](#示例) · [已知限制](#已知限制) · [构建与测试](#构建与测试)

下面是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中移动 Actor 的部分：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self 是绑定这个脚本的 Actor。
    FVector position = UE.Self.GetActorLocation();

    // 沿 X 轴移动，速度为每秒 120 UE 单位。
    UE.Self.SetActorLocation(position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`avid_on_tick` 对应 UE 的每帧 Tick；`deltaSeconds` 为这一帧的时长，单位为秒。

## 快速开始

需要：

- Unreal Engine 5.8 源码版
- Visual Studio 2022，安装 UE C++ 开发工作负载
- PowerShell 7
- .NET SDK **8.0.416**，版本固定在 [global.json](global.json)

以下命令假定已有 `AvidTPSTemplate` 工程，且本仓库位于其 `Plugins/AvidScript` 目录。打开 PowerShell，进入该目录：

```powershell
# 安装 Wasmtime 运行库
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

```powershell
# 构建 Editor target，将此路径改为本机 UE 源码目录
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开 `AvidTPSTemplate.uproject`。
2. 放置一个 Cube，设为 **Movable** 并选中，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转、放大。

修改 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs` 后，再执行一次菜单命令即可重新编译并绑定。

## 示例

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay`、`Tick`、输入和碰撞回调 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 延时与取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端与服务器 RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性复制与 `RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 与存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目里的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 用 C# 声明 Actor、Component、Subsystem |

<details>
<summary>查看异步示例：延时后缩放 Actor</summary>

摘自 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：

```csharp
private static AvidCancellationSource LifetimeCancellation;

[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    LifetimeCancellation = AvidCancellationSource.Create();

    // 等待 0.25 秒，再缩放 Actor。
    await UKismetSystemLibrary.DelayAsync(0.25f)
        .WithCancellation(LifetimeCancellation.Token);
    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}

[UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
public static void EndPlay()
{
    // Actor 结束运行时，取消尚未完成的等待。
    LifetimeCancellation.Cancel();
    LifetimeCancellation.Release();
}
```

编译和运行步骤见 [LatentGameplay](Samples/CSharp/LatentGameplay/README.md)。

</details>

## 已知限制

- 仅支持部分 C# 语法，不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 仅支持同一脚本实例内等待。外层 `await` 未处理的取消仍可能中止脚本执行。
- 在 `try` 中直接 `await` 需要[预览开关](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)；`catch`、`finally` 内尚不支持 `await`。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。
- Android、iOS、Shipping 和真实多人游戏尚未验收。

相关文档：[跨 `await` 使用对象的测试脚本](Fixtures/Phase66/DirectAwaitCleanup.cs)（需上述预览开关） · [脚本热重载与失败回滚测试](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md#独立-session-的代码更新与回滚) · [Task 说明](Docs/Phase66/P66.C4_Task_Result_Contract.md) · [测试命令与结果](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

## 构建与测试

在仓库根目录执行。只编译 ActorLifecycle 脚本、生成 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

运行 C# 到 WASM 的编译工具测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

## 项目结构

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

```text
Source/    UE 插件模块
Tools/     C# 编译器、中间表示、WASM 生成工具
Build/     构建与验证脚本
Samples/   示例脚本
Docs/      使用说明、设计与测试记录
```

开发约定：[AGENTS.md](AGENTS.md)。

## 许可

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
