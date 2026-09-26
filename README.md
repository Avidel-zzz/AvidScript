# AvidScript

<img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="720">

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，通过生成的绑定调用 UE API；游戏进程不加载 CLR。

**开发中。** 当前支持部分 C# 语法，主要验证 UE 5.8 / Win64 Editor 与 Development。

[安装](#安装) · [样例](#样例) · [限制](#限制) · [开发](#开发)

## 示例

每帧移动脚本绑定的 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前 Actor，`avid_on_tick` 将方法注册为每帧回调。上面的代码以每秒 120 UE 单位沿 X 轴移动，完整脚本见 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

<details>
<summary>异步示例：等待 0.25 秒后缩放 Actor，在 EndPlay 时取消等待</summary>

摘自 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：

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

构建与运行步骤见 [LatentGameplay](Samples/CSharp/LatentGameplay/README.md)。

</details>

## 安装

以下步骤使用 `AvidTPSTemplate` 工程。需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。

在 `Plugins/AvidScript` 目录执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$ueRoot = 'C:\UnrealEngine' # 本机 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开 `AvidTPSTemplate.uproject`，放置一个 Cube，将 Mobility 设为 **Movable** 并选中它。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转、放大。

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，重新执行第 2 步即可编译并绑定。也可以只生成样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 样例

| 功能 | 源码 / 说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 延时与取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 限制

- C# 支持尚不完整，不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 只支持同一脚本实例内等待。默认构建仍不支持在 `try/catch` 中直接 `await`；外层 `await` 未处理的取消目前会使 VM 中止执行。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。
- Android、iOS、Shipping 和真实多人游戏尚未验收。

直接 `await` 与 Task 的 `try/catch/finally` 组合可通过[预览开关](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)使用；取消清理中的异常会传给外层等待者。其他异步限制见 [Task 结果](Docs/Phase66/P66.C4_Task_Result_Contract.md)；Win64 Wasmtime / WAMR 的异步、数组、泛型组合测试见[验证记录](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

## 开发

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

| 目录 | 内容 |
| --- | --- |
| [Tools/](Tools/) | C# 前端、Guest IR、WASM 生成工具 |
| [Source/](Source/) | UE 插件模块 |
| [Build/](Build/) | 构建与验证脚本 |
| [Docs/](Docs/) | 设计、使用说明与测试记录 |

运行 Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

开发约定见 [AGENTS.md](AGENTS.md)。

## 许可

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
