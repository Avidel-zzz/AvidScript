# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 UE 5.8 插件和 C# → WASM 编译工具链。脚本通过生成的绑定调用 UE API。当前主要验证 Win64。

![C# 到 Unreal Engine 的编译与调用路径](Docs/Assets/README/pipeline.svg)

## Build & run

依赖：UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、[global.json](global.json) 指定的 .NET SDK。

1. 在 `Plugins/AvidScript` 安装锁定版本的 Wasmtime：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

2. 构建 Editor target（将 `UE_ROOT` 改为本机引擎目录）：

```powershell
$env:UE_ROOT = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

3. 打开 `AvidTPSTemplate.uproject`。在关卡中放置并选中一个 **Movable** Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，再进入 **Play**。Cube 会移动、旋转并放大。

只构建该样例的 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## C# examples

每帧沿 X 轴移动 120 UE 单位（摘自 [`ActorLifecycleScript.cs`](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)）：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

等待 0.25 秒后修改缩放；`EndPlay` 时取消未完成的等待（摘自 [`LatentGameplayScript.cs`](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)）：

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

![Delay 完成与 EndPlay 取消的执行路径](Docs/Assets/README/async-lifecycle.svg)

## Samples

| 要做的事 | 样例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Limits

- 只编译已支持的 C# 和 UE API 子集；任意 .NET 项目、NuGet 包不能直接运行。
- `Task<int>`：受限的同源静态调用与 `await` 可用；跨 Session 等待未实现。见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `catch/throw`：构建时指定 `-LanguageErrors bounded`。生成类型使用此模式目前限 Win64 Development。异步方法内抛错、`await` 后捕获、`finally` 中的 `await` 尚不能生成可运行 WASM。见 [语言错误合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改脚本定义的 `UClass`、`UProperty`、`UFunction` 后需重新构建并重启 Editor。
- Android、iOS、真实多人联机、完整游戏流程仍待验收。

## Development

运行 Guest 工具链测试（在插件根目录）：

```powershell
& (Join-Path $env:USERPROFILE '.dotnet\dotnet.exe') run `
  --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

[`Source/`](Source/) 是 UE 插件；[`Tools/`](Tools/) 是编译器；[`Build/`](Build/) 是构建脚本；[`Docs/`](Docs/) 保存设计和验证记录。仓库开发规则见 [AGENTS.md](AGENTS.md)。

## License

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
