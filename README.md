# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。C# 源码编译为 WebAssembly；UE 插件加载模块，并通过生成的绑定调用引擎 API。游戏运行时不需要 CLR。

![C# 源码经编译器生成 WASM，由 UE Runtime 调用 UObject](Docs/Assets/README/pipeline.svg)

目前主要在 **UE 5.8 源码版 / Win64** 上开发和验证。编译器支持的是已实现的 C# 子集，不能直接运行任意 .NET 项目或 NuGet 包。

## 运行 Actor 样例

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK。下面的命令均在 `Plugins/AvidScript` 目录执行。

1. 安装 Win64 Wasmtime 依赖：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
   ```

2. 构建 Editor（将 `UE_ROOT` 改为你的引擎目录）：

   ```powershell
   $env:UE_ROOT = "C:\Path\To\UnrealEngine"
   $uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
   & (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
     AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
     -WaitMutex -NoHotReloadFromIDE
   ```

3. 打开工程，在关卡中放置并选中一个 **Movable** Cube。选择 **Tools > AvidScript > Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 会移动、旋转并放大。

只需生成样例 WASM 时，运行 `pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1`。如果 Play 后 Cube 没变化，检查它是否挂有 **AvidScript Component**，并在 **Output Log** 中搜索 `AvidScript`。

## C# 示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 使用当前 Actor 的位置。以下调用让它每秒沿 X 轴移动 120 个 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

也可以在 C# 中定义供 Blueprint 使用的类型。[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 的 `Projectile` 声明了属性和异步函数：

```csharp
[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public async void SetLaunchSpeedNextTick(float speed)
    {
        await AvidContinuations.NextTickAsync();
        LaunchSpeed = speed;
    }
}
```

调用 `SetLaunchSpeedNextTick(900)` 后，属性在下一帧更新为 `900`。新增或修改 `UProperty`、`UFunction` 等反射声明后，需要重新构建并重启 Editor；见[类型定义样例](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 更多样例

| 想做什么 | 从这里开始 |
| --- | --- |
| Actor 生命周期、位置与旋转 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 等待下一帧、计时器和异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| Server/Client RPC、属性复制和 RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 和存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 绑定项目中的 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 定义 Actor、Component 和 Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 当前状态

- Win64 Editor 和打包样例已有自动化验证。Android 真机和 iOS 尚未验收。
- 网络 RPC、属性复制和 RepNotify 有独立进程自动化测试；实际游戏中的多人联机仍需单独验收。
- `try` / `catch` / `finally` 还不能用于普通脚本构建。专用测试入口已验证 `try { return Get(); } catch (System.Exception) { return 7; } finally { Count = Count + 1; }` 等受限写法；详见[实现范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)。

代码位于 [`Source/`](Source/)（UE 插件）和 [`Tools/`](Tools/)（C# 编译器）；构建脚本见 [`Build/`](Build/)，设计与验证记录见 [`Docs/`](Docs/)。仓库开发约定见 [AGENTS.md](AGENTS.md)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
