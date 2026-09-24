# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。构建工具将支持的 C# 代码编译为 WASM；UE 插件加载 WASM，并通过生成的绑定调用引擎 API。游戏进程不加载 CLR。目前主要在 Win64 上开发和验证。

![C# 到 WASM，再到 UE Runtime 的调用路径](Docs/Assets/README/pipeline.svg)

## Example

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 读取 Actor 位置，并按每秒 120 UE 单位的速度沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定到脚本的 Actor；`deltaSeconds` 来自 UE Tick。完整样例还处理输入、碰撞和异步资源加载。

## Quick start

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK 8.0.416。以下命令针对仓库中的模板工程，在 `Plugins/AvidScript` 目录执行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine" # 改为本机的 UE 5.8 源码目录
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开工程，将一个 `Movable` Cube 放进关卡并选中它。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，再点击 **Play**。Cube 应移动、旋转并放大。如果没有反应，检查 Cube 上的 `AvidScript Component`，并在 Output Log 中搜索 `AvidScript`。

只编译这个样例的 WASM：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## Samples

| 用途 | 代码与说明 |
| --- | --- |
| 处理 Actor 生命周期、输入和碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 等待资源加载、Timer 和 Latent 回调 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| 使用 RPC、属性复制和 RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| 制作 UI 和存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 用 C# 定义 Actor、Component 和 Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## Current limits

AvidScript 仍在开发中。Win64 Editor、打包样例和独立进程网络样例有自动化测试；真实游戏流程、真实多人联机及 Android/iOS 还没有完成验收。

- 编译器只支持已实现的 C# 和 UE API 子集。现有 .NET 项目或 NuGet 包不能直接作为游戏脚本运行。
- `Task<int>` 可以直接 `await` 同源静态方法，例如 [测试用例](Tools/AvidScript.CSharpGuest.Tests/CSharpGuestAsyncInvocationTests.cs)中的 `int score = await LoadScoreAsync(7, 5)`；值参数按声明顺序传递，取消会向上层传播。Task 变量和其他 `Task<T>` 尚不支持。细节见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 普通脚本可用同步 `try/finally`，包括 `await` 恢复后的同步代码；`await` 放在 `try/finally` 内仍不支持。`catch` / `throw` 只在[专用测试入口](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)支持受限写法。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。参见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## Development

运行 C# Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

UE 插件代码在 [Source/](Source/)，C# 工具链在 [Tools/](Tools/)，构建脚本在 [Build/](Build/)。贡献约定见 [AGENTS.md](AGENTS.md)，设计和验证记录见 [Docs/](Docs/)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不在本仓库内。
