# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 UE 5.8 的 C# 脚本插件：将受支持的 C# 代码编译为 WASM，在 UE 中加载并调用生成的引擎绑定。游戏进程不加载 CLR。当前以 **Win64 Editor / Development** 为验证目标。

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

## 快速开始

- UE 5.8 源码版
- Visual Studio 2022，安装 UE C++ 工作负载
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

在 `Plugins/AvidScript` 目录安装 Wasmtime 依赖并构建 Editor：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$ueRoot = 'C:\UnrealEngine' # 改为本机 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable Cube**。
2. 选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 Play。Cube 会移动、旋转、放大。修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，重新执行第 2 步。

只生成样例 WASM，不启动 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 代码示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每秒沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 展示带取消令牌的 `await`；`EndPlay` 发出取消并释放令牌源：

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

## 样例索引

| 要做什么 | 样例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 当前状态

| 范围 | 验证情况 |
| --- | --- |
| 模板工程，Win64 Editor / Development | 构建与 ActorLifecycle 样例已验证 |
| 异步、数组与泛型组合 | [集成样例](Fixtures/Phase66/IntegratedLanguageFlow.cs)通过 Win64 Wasmtime/WAMR 自动化；见[验证记录](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| Android / iOS / Shipping | 尚未验收 |
| 真实多人游戏 | 尚未验收；仓库提供 RPC 与属性复制样例 |

当前限制：

- 编译器只支持项目实现的 C# 子集，不能直接运行任意 .NET 程序或 NuGet 包。
- `Task<int>` 只支持同一脚本实例内等待，见 [Task 结果](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 直接在 `try/catch` 内 `await` 仍不支持；主动取消后，未处理的外层 `await` 目前会触发 VM trap。见[当前边界](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)。
- C# 声明的 `UClass`、`UProperty` 或 `UFunction` 变更后，需要重新构建并重启 Editor。

## 开发

[`Tools/`](Tools/) 是 C# 编译工具，[`Source/`](Source/) 是 UE 插件模块，[`Build/`](Build/) 是构建与验证脚本。设计和验证记录在 [`Docs/`](Docs/)；开发约定见 [`AGENTS.md`](AGENTS.md)。

运行 Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

## 许可

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
