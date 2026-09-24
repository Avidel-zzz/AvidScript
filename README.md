# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是面向 Unreal Engine 5.8 的 C# 脚本插件。构建时将受支持的 C# 编译为 WebAssembly；运行时通过生成的绑定调用 UE API，游戏进程无需加载 CLR。

![C# 源码、编译器、WASM 与 UE Runtime 的调用路径](Docs/Assets/README/pipeline.svg)

## Example

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 节选：每秒沿 X 轴移动 120 UE 单位。`UE.Self` 是绑定到脚本的 Actor，`deltaSeconds` 来自 UE Tick。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

## Build and run

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [global.json](global.json) 指定的 .NET SDK 8.0.416。以下命令从 `Plugins/AvidScript` 执行，并假设模板工程位于 `../../AvidTPSTemplate.uproject`。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine" # 改为本机 UE 5.8 源码目录
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开模板工程，在关卡中放入并选中一个 `Movable` Cube，然后运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。点击 **Play** 后，Cube 会移动、旋转并放大。若没有反应，检查 Cube 的 `AvidScript Component`，并在 Output Log 中搜索 `AvidScript`。

只编译示例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Samples

| 用途 | 示例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 异步加载、Timer、Latent | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Current limits

目前主要验证 Win64 Editor、打包样例和独立进程网络样例。真实游戏流程、真实多人联机及 Android/iOS 尚未验收。

- 只支持已实现的 C# 和 UE API 子集；不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 可直接等待同源静态方法，例如 `int score = await LoadScoreAsync(7, 5)`；Task 变量和其他 `Task<T>` 还不支持。见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `try/finally` 可用，但 `try/finally` 内不能 `await`。`catch` / `throw` 尚未接入常规构建入口。见 [异常合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## Development

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

[Source/](Source/) 是 UE 插件，[Tools/](Tools/) 是 C# 工具链，[Build/](Build/) 是构建脚本。设计和验证记录在 [Docs/](Docs/)，仓库约定见 [AGENTS.md](AGENTS.md)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不在本仓库内。
