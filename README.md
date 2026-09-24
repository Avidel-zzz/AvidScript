# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

UE 5.8 的 C# 脚本插件。编译器将受支持的 C# 代码转成 WebAssembly；插件通过生成的绑定调用 UE API，游戏进程不加载 CLR。目前主要在 Win64 开发和验证。

![C# 源码、WASM 与 UE Runtime 的调用路径](Docs/Assets/README/pipeline.svg)

## 快速开始

以下步骤使用插件所在工程的 `AvidTPSTemplate.uproject`。需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 中的 .NET SDK 8.0.416。在 `Plugins/AvidScript` 目录运行：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine" # 改成你的 UE 5.8 源码目录
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开工程，在关卡中放入一个设为 `Movable` 的 Cube 并选中。点击 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后进入 Play。Cube 应沿 X 轴移动，同时旋转、放大。没有反应时，检查 Cube 上的 `AvidScript Component`，并在 Output Log 中搜索 `AvidScript`。

只编译样例 WASM，无需打开 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 脚本示例

下面是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick`，省略了同一方法中的旋转和缩放代码。`UE.Self` 是当前绑定的 Actor，`deltaSeconds` 来自 UE 的 Tick。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

同一文件的 `BeginPlay` 还使用 `await AvidAssets.LoadObjectAsync(...)` 和 `await AvidContinuations.NextTickAsync()`。等待 UE latent 调用并主动取消的示例见 [LatentGameplay](Samples/CSharp/LatentGameplay/README.md)。

## 更多样例

| 场景 | 入口 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Latent 调用与取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md)、[ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 定义 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 当前范围

- 这是开发中的插件。Win64 Editor、打包样例及独立进程网络样例有自动化测试；真实游戏流程、真实多人联机和 Android/iOS 尚未验收。
- 编译器实现的是 C# 与 UE API 的子集，不支持把任意 .NET 项目或 NuGet 包直接放进游戏运行。
- `Task<int>` 仅支持直接 `await` 同源静态无参数方法；Task 变量、其他 `Task<T>` 以及语言错误传播还未接通，见 [任务结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- `try` / `catch` / `finally` 的受限写法目前只在[专用测试入口](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)可用。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor，见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 开发

运行 C# Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

UE 模块在 [Source/](Source/)，C# 工具链在 [Tools/](Tools/)，构建入口在 [Build/](Build/)。仓库约定见 [AGENTS.md](AGENTS.md)，详细设计与验证记录见 [Docs/](Docs/)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不在本仓库内。
