# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

在 Unreal Engine 5.8 中运行 C# 游戏脚本。AvidScript 将受支持的 C# 代码编译为 WebAssembly，通过生成的绑定调用 UE API；游戏运行时不加载 CLR。目前以 Win64 为主要开发和验证平台。

![C# 到 WASM，再到 UE Runtime 的调用路径](Docs/Assets/README/pipeline.svg)

## Example

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每秒将 Actor 沿 X 轴移动 120 UE 单位：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向绑定的 Actor；`deltaSeconds` 由 UE 的 Tick 传入。完整样例还包含异步加载、输入和碰撞回调。

## Build and run

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK 8.0.416。以下命令在 `Plugins/AvidScript` 目录执行，使用仓库中的 `AvidTPSTemplate.uproject`：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine" # 改为本机的 UE 5.8 源码目录
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开工程，在关卡中放入一个设为 `Movable` 的 Cube 并选中它。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 应移动、旋转并放大。若没有变化，检查 Cube 上的 `AvidScript Component`，并在 Output Log 搜索 `AvidScript`。

只构建样例 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Samples

| 用途 | 样例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 异步加载、Timer、Latent、取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Status

这是开发中的插件。Win64 Editor、打包样例和独立进程网络样例有自动化测试；真实游戏流程、真实多人联机及 Android/iOS 尚未验收。

- 编译器支持项目已实现的 C# 与 UE API 子集。任意 .NET 项目和 NuGet 包不能直接作为游戏脚本运行。
- `Task<int>` 支持直接 `await` 同源静态方法并按声明顺序传入值参数。Task 变量和其他 `Task<T>` 尚未支持；异步错误还不能按普通 C# 的 `catch` / `finally` 语义处理。详见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- `try` / `catch` / `finally` 的受限写法目前只在[专用测试入口](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)可用。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。详见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## Development

运行 C# Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

UE 插件代码在 [Source/](Source/)，C# 工具链在 [Tools/](Tools/)，构建入口在 [Build/](Build/)。仓库约定见 [AGENTS.md](AGENTS.md)，设计与验证记录见 [Docs/](Docs/)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不在本仓库内。
