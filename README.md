# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 将支持的 C# 脚本编译为 WASM，在 Unreal Engine 5.8 中执行。UE API 通过生成的绑定调用，游戏进程不加载 CLR。当前目标平台为 Win64。

![C# 源码经 Roslyn 和 Guest IR 编译为 WASM，再由 UE Runtime 调用 UObject](Docs/Assets/README/pipeline.svg)

## Code

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向绑定的 Actor，`deltaSeconds` 由 UE Tick 传入。完整样例还包含输入、碰撞和异步资源加载。

## Quick start

环境：UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7、[.NET SDK 8.0.416](global.json)。以下命令在 `Plugins/AvidScript` 目录执行，使用仓库自带的 `AvidTPSTemplate.uproject`。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine" # 本机 UE 5.8 源码目录
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开工程，在关卡中放入并选中一个设为 `Movable` 的 Cube。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，再点击 **Play**。Cube 会移动、旋转并放大。

只编译样例 WASM，无需打开 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

没有看到 Cube 运动时，检查其 `AvidScript Component`，并在 Output Log 中搜索 `AvidScript`。

## Samples

| 场景 | 示例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 异步加载、Timer、Latent 调用 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) / [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Status

AvidScript 处于开发阶段。Win64 Editor、打包样例和独立进程网络样例已有自动化测试；真实游戏流程、真实多人联机和 Android/iOS 尚未验收。

- 编译器支持 C# 和 UE API 的子集；不能直接运行任意 .NET 项目或 NuGet 包。
- 异步支持 `int score = await LoadScoreAsync(7, 5)` 这样的[同源静态 `Task<int>` 调用](Tools/AvidScript.CSharpGuest.Tests/CSharpGuestAsyncInvocationTests.cs)；UE 事件回调内直接等待 `Task<int>` 也已在 Win64 双 VM 验证。Task 变量和其他 `Task<T>` 尚不支持；详见 [Task<int> 合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `try/finally` 可用，包括 `await` 恢复后的代码；`try/finally` 内的 `await` 尚不支持。`catch` / `throw` 目前只在[专用测试入口](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)支持受限写法。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需重新构建并重启 Editor。

## Development

运行 C# Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

目录：[Source/](Source/)（UE 插件）、[Tools/](Tools/)（C# 工具链）、[Build/](Build/)（构建脚本）、[Docs/](Docs/)（设计与验证）。仓库约定见 [AGENTS.md](AGENTS.md)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不在本仓库内。
