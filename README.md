# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

在 Unreal Engine 5.8 中运行 C# 游戏脚本。构建工具将 C# 编译成 WASM；游戏运行时加载 WASM，通过生成的绑定调用 UE API。

![C# 到 UE 对象的编译与调用流程](Docs/Assets/README/pipeline.svg)

## 看一段代码

在 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中，`Tick` 每秒让绑定的 Actor 沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

异步代码也写在脚本里。[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 等待 0.25 秒后修改 Actor 缩放；`EndPlay` 会取消等待：

```csharp
LifetimeCancellation = AvidCancellationSource.Create();
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
```

## 快速开始（Win64）

需要 UE 5.8 源码版、Visual Studio 2022 的 UE C++ 工作负载、PowerShell 7 和 [仓库指定的 .NET SDK](global.json)。在 `Plugins/AvidScript` 目录运行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 换成你的 UE 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放一个 **Movable** Cube 并选中它。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。

只编译这个样例的 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 更多样例

| 想做什么 | 从这里开始 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目里的 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 从 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 当前边界

- 编译器实现的是 C# / UE API 子集，不能直接运行普通 .NET 项目或任意 NuGet 包。
- `Task<int>` 可直接 `await`，也可存入局部变量或别名；前一次 `await` 后、提前退出的条件分支内都能创建任务。所有权不确定的分支汇合、任务变量重赋值和其他 `Task<T>` 尚未支持。详见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `try/finally` 可用。默认构建仍拒绝 `catch`/`throw`；`-LanguageErrors bounded` 接受受限同步异常，可用于[生命周期脚本](Fixtures/Phase66/BoundedLanguageErrorsLifecycle.cs)和[生成 Actor 的 UFunction](Fixtures/Phase66/BoundedGeneratedType.cs)。生成类型的这一模式目前限 Win64 Development；`finally` 内的 `await` 尚未支持。详见[语言错误合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。
- Win64 Editor、打包样例和独立进程网络样例有自动化测试；真实游戏流程、真实多人联机及 Android/iOS 尚待验收。

## 开发

运行 C# Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

UE 插件代码在 [Source/](Source/)，C# 工具链在 [Tools/](Tools/)，构建脚本在 [Build/](Build/)。实现与验证记录在 [Docs/](Docs/)；修改代码前请读 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
