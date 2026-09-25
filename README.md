# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

在 Unreal Engine 5.8 中运行 C# 游戏脚本。工具链用 Roslyn 解析受支持的 C#，生成 WASM；插件在 UE 内执行 WASM，通过绑定访问 Actor 和引擎 API。UE 进程不加载 CLR。

**当前状态：**开发中，以下步骤针对仓库自带模板工程的 Win64 Editor / Development。Android、iOS、Shipping 和真实多人游戏尚未完成验收。

![C# 源码到 UE 对象的路径](Docs/Assets/README/pipeline.svg)

## 快速开始

环境：UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、[.NET SDK 8.0.416](global.json)。命令均在 `Plugins/AvidScript` 目录执行。

1. 安装仓库锁定的 Wasmtime 依赖：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建模板工程的 Editor target。按本机路径设置 `UE_ROOT`：

   ```powershell
   $env:UE_ROOT = 'C:\UnrealEngine'
   $project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
   & (Join-Path $env:UE_ROOT 'Engine\Build\BatchFiles\Build.bat') `
     AvidTPSTemplateEditor Win64 Development "-Project=$project" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 打开 `AvidTPSTemplate.uproject`，在关卡中放置并选中一个 **Movable** Cube。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，进入 **Play**。Cube 会移动、旋转并逐渐放大。修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，重新执行同一菜单项即可构建并绑定新版本。

单独构建样例 WASM（不启动 Editor）：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 代码示例

下面的 `Tick` 来自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。`deltaSeconds` 是本帧时长；每秒沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

异步等待见 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：`BeginPlay` 等待 0.25 秒后放大 Actor；`EndPlay` 取消尚未完成的等待。

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

## 样例索引

| 要看什么 | 入口 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer、异步加载、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 已知限制

- 支持的是 C# 和 UE API 的子集；不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 仅支持同一脚本实例内的等待；跨实例等待尚未支持。见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 显式 `-LanguageErrors bounded` 可处理受支持的同步 `throw/catch` 和异步 `Task<int>` 故障；`await` 后的 `catch/finally` 仍未进入正式可执行构建。见 [异步异常合同](Docs/Phase66/P66.C5_Async_Language_Error_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

运行 Guest 工具链测试：

```powershell
& (Join-Path $env:USERPROFILE '.dotnet\dotnet.exe') run `
  --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

代码在 [Source/](Source/)（UE 插件）和 [Tools/](Tools/)（C# 工具链）；构建入口在 [Build/](Build/)，更多设计与验证记录在 [Docs/](Docs/)。仓库开发约定见 [AGENTS.md](AGENTS.md)。

## 许可证

本仓库原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
