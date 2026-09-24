# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。构建工具将受支持的 C# 编译为 WebAssembly；UE 插件加载 WASM，并通过生成的绑定调用 UE API。游戏运行时不加载 CLR。

![C# → Roslyn → Guest IR → WASM → UE Runtime](Docs/Assets/README/pipeline.svg)

## 快速开始

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在 `Plugins/AvidScript` 目录执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine" # 换成你的 UE 5.8 源码目录
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 `AvidTPSTemplate.uproject`，在关卡中放一个设为 `Movable` 的 Cube，选中它，运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，再点击 **Play**。Cube 应移动、旋转并放大。菜单使用的脚本是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

只构建样例 WASM 时，无需启动 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## C# 代码示例

以下是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick`。`UE.Self` 指向绑定了脚本组件的 Actor；`deltaSeconds` 是本帧经过的秒数。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
    FRotator currentRotation = UE.Self.GetActorRotation();
    UE.Self.SetActorRotation(currentRotation + new FRotator(0.0f, 90.0f * deltaSeconds, 0.0f));
    FVector currentScale = UE.Self.GetActorScale3D();
    UE.Self.SetActorScale3D(currentScale + new FVector(0.0f, 0.0f, 0.6f * deltaSeconds));
}
```

异步调用见 [LatentGameplay](Samples/CSharp/LatentGameplay/README.md)：`await UKismetSystemLibrary.DelayAsync(0.25f)` 在等待期间挂起脚本，恢复后再修改 Actor。该样例也展示了 EndPlay 时取消等待。

## 样例

| 用途 | 源码或说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 异步资源加载、Timer、Latent | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 在 C# 中声明 UE 类型 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 支持范围

- **已自动测试：** Win64 Editor、打包样例和独立进程网络样例。真实游戏流程、真实多人联机、Android 和 iOS 仍需验收。
- **C#：** 只编译已实现的语法与 UE API 子集；不能直接运行任意 .NET 项目或 NuGet 包。
- **异步结果：** 支持直接 `await LoadScoreAsync(7, 5)`；也支持方法首句声明一个 `Task<int> pending = LoadScoreAsync()`，跨帧后 `await pending`。多个 Task 变量、重赋值和其他 `Task<T>` 暂不支持。详见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- **异常：** 同步 `try/finally` 可用；`await` 不能放在其中。`catch` / `throw` 尚未接入常规构建入口。详见 [异常合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- **UE 类型声明：** 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需重新构建并重启 Editor。

## 开发

运行 C# Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

`Source/` 包含 UE 插件，`Tools/` 包含 C# 工具链，`Build/` 包含构建脚本。设计与验证记录见 [Docs/](Docs/)；仓库开发约定见 [AGENTS.md](AGENTS.md)。

## License

插件原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
