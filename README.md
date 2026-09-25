# AvidScript

![AvidScript: C# scripts for Unreal Engine](Docs/Assets/README/avidscript-hero.svg)

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 5.8 的 C# 脚本插件。C# 在构建时编译为 WASM；UE 插件加载 WASM 并调用生成的 UE 绑定。当前主要支持 Win64，项目仍在开发中。

## 快速开始

环境：UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、[.NET SDK 8.0.416](global.json)。以下命令从 `Plugins/AvidScript` 执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = 'C:\UnrealEngine' # 换成你的 UE 5.8 源码目录
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject` → 在关卡放入并选中一个 **Movable** Cube → 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script** → 点击 **Play**。Cube 应移动、旋转并放大。源码：[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

只编译该样例的 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## C# 示例

`Tick` 中通过 `UE.Self` 访问绑定的 Actor。以下代码来自 `ActorLifecycleScript.cs`，沿 X 轴以 120 UE 单位/秒移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

下面的代码简化自 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：`BeginPlay` 等待 0.25 秒后修改 Actor；`EndPlay` 取消等待。

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

## 示例目录

| 场景 | 文件 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 异步加载、Timer、Latent 与取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| 等待 `Task<int>` 并组合数组、泛型、`finally` | [TaskIntIntegrated.cs](Fixtures/Phase66/TaskIntIntegrated.cs) |

## 构建路径

![C# 源码、WASM 与 UE Runtime 的关系](Docs/Assets/README/pipeline.svg)

Roslyn 只在构建时运行。WASM 通过 `ObjectHandle` 和生成的绑定访问 UE 对象；UE Runtime 不加载 CLR。

## 支持范围

- Win64 Editor、打包样例和独立进程网络样例已通过自动化测试。真实游戏流程、真实多人联机及 Android/iOS 尚未验收。
- 编译器只支持已实现的 C# 和 UE API 子集，不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 可直接 `await`，或将结果赋给已声明的 `int` 局部变量、脚本静态 `int` 字段；方法开头最多可声明 8 个独立任务变量供跨帧等待。任务别名、任务变量重赋值及其他 `Task<T>` 尚未支持。详见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `try/finally` 可用；`finally` 内的 `await`、常规构建入口中的 `catch` 和 `throw` 尚未支持。详见 [异常合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

从插件根目录运行 Guest 工具链测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

[Source/](Source/) 是 UE 插件，[Tools/](Tools/) 是 C# 工具链，[Build/](Build/) 是构建脚本。设计与验证记录见 [Docs/](Docs/)；参与开发前请阅读 [AGENTS.md](AGENTS.md)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
