# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# → WebAssembly 脚本插件。C# 在构建时编译；游戏运行时通过生成的绑定访问 UE API，不加载 CLR。

![C# 源码经 Roslyn 和 Guest IR 编译为 WASM，再由 UE Runtime 调用 UObject](Docs/Assets/README/pipeline.svg)

## 示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick`：`UE.Self` 是挂载脚本的 Actor，下面的代码让它每秒沿 X 轴移动 120 UE 单位。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

## 快速开始

需要 UE 5.8 源码版、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK 8.0.416。以下命令在 `Plugins/AvidScript` 目录执行；模板工程位于 `../../AvidTPSTemplate.uproject`。

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

$env:UE_ROOT = "C:\UnrealEngine" # 改成你的 UE 5.8 源码目录
$uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
& (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
  AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

打开模板工程，在关卡中放入一个设为 `Movable` 的 Cube 并选中它。执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 应开始移动、旋转和放大。若没有反应，检查它的 `AvidScript Component`，并在 Output Log 中搜索 `AvidScript`。

只编译这个样例的 WASM，不启动 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

## 更多样例

| 主题 | 示例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 等待资源加载、Timer、Latent 调用 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目里的 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| 在 C# 中声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

## 当前状态

Win64 Editor、打包样例和独立进程网络样例已有自动化覆盖。真实游戏流程、真实多人联机及 Android/iOS 尚未验收。

- 编译器只支持已实现的 C# 和 UE API 子集；不能直接运行任意 .NET 项目或 NuGet 包。
- `Task<int>` 支持直接等待同源静态方法，例如 `int score = await LoadScoreAsync(7, 5)`；Task 变量及其他 `Task<T>` 尚不支持。见 [Task 结果合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)。
- 同步 `try/finally` 可用，但其中不能 `await`。`catch` / `throw` 尚未接入常规构建入口。见 [异常合同](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)。
- 修改 C# 声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor。

## 开发

运行 C# Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

`Source/` 是 UE 插件，`Tools/` 是 C# 工具链，`Build/` 是构建脚本。设计和验证记录在 [Docs/](Docs/)；仓库工作约定见 [AGENTS.md](AGENTS.md)。

## License

插件原创代码采用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不在本仓库内。
