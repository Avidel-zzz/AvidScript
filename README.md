# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件：将受支持的 C# 编译为 WebAssembly，在 UE 中执行，并通过生成的绑定调用引擎 API。UE 运行时不加载 CLR。

> 开发中。下方命令适用于仓库自带的模板工程和 Win64 Editor / Development；Android、iOS 与真实多人游戏尚未验收。

![C# 到 Unreal Engine 的编译与运行路径](Docs/Assets/README/pipeline.svg)

## 代码示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)：Actor 每秒沿 X 轴移动 120 UE 单位。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：等待 0.25 秒后放大 Actor；`EndPlay` 取消尚未完成的等待。

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

![等待完成与 EndPlay 取消](Docs/Assets/README/async-lifecycle.svg)

## 快速开始

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [global.json](global.json) 指定的 .NET SDK。以下命令在 `Plugins/AvidScript` 目录运行。

1. 安装锁定版本的 Wasmtime：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建模板工程的 Editor target。将 `UE_ROOT` 改为本机 UE 源码目录：

   ```powershell
   $env:UE_ROOT = 'C:\UnrealEngine'
   $project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
   & (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
     AvidTPSTemplateEditor Win64 Development "-Project=$project" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 打开 `AvidTPSTemplate.uproject`。在关卡中放置并选中一个 **Movable** Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，再进入 **Play**。Cube 会移动、旋转并放大。

只生成样例 WASM，不启动 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 更多样例

| 场景 | 源码与说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md)、[ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 当前限制

- 编译器支持的是 C# 子集和部分 UE API；不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 可在同一脚本实例内等待，不能跨实例等待（[说明](Docs/Phase66/P66.C4_Task_Result_Contract.md)）。
- 同步代码支持部分 `throw/catch`；`await` 后的 `catch` 和异步 `finally` 尚不支持（[说明](Docs/Phase66/P66.C5_Async_Language_Error_Contract.md)）。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

运行 Guest 工具链测试：

```powershell
& (Join-Path $env:USERPROFILE '.dotnet\dotnet.exe') run `
  --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

[Source/](Source/) 是 UE 插件，[Tools/](Tools/) 是 C# 工具链，[Build/](Build/) 是构建入口，[Samples/](Samples/) 是可运行样例，[Docs/](Docs/) 包含设计与验证记录。贡献和开发规则见 [AGENTS.md](AGENTS.md)。

## 许可证

原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
