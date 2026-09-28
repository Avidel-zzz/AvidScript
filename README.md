# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white)
![C# to WASM](https://img.shields.io/badge/C%23-to%20WASM-512BD4?logo=csharp&logoColor=white)
![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white)
[![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

UE 5.8 的实验性 C# 脚本插件。构建时把 C# 编译为 WebAssembly；游戏运行时不加载 CLR。当前主要在 Win64 Editor / Development 上开发和验证。

![C# 源码到 UE Actor 的编译与运行路径](Docs/Assets/README/pipeline.svg)

## 快速开始

需要 **UE 5.8 源码版 C++ 工程**、Visual Studio 2022 的 UE C++ 工具链、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在工程根目录运行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译工程的 Editor target；把路径和 `YourGame` 换成自己的工程：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../YourGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  YourGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 Editor 后，从 **Tools → AvidScript**：

1. 执行 **Create Project C# Gameplay Workspace**。项目中会生成 `Scripts/AvidScript/GameplayScript.cs`；再次执行不会覆盖已修改的脚本。
2. 编辑这个文件，选中关卡里带可移动 RootComponent 的 Actor。
3. 执行 **Build And Bind Project C# Gameplay Script**，然后点击 **Play**。

这个入口会生成绑定 API、编译脚本并绑定选中的 Actor。[项目脚本使用说明](Docs/Phase44/P44.3_Project_CSharp_Gameplay_Workspace.md)列出了生成文件和完整操作。
修改脚本后，停止 Play，重新执行 Build And Bind。

只想先看效果，可以选中一个 **Movable** Cube，执行 **Build And Bind C# ActorLifecycle Script**，再点击 **Play**。该示例会移动、旋转和缩放 Cube；源码在 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

## 写脚本

项目工作区生成的 `GameplayScript.cs` 有 `BeginPlay` 和 `Tick` 入口。把其中的 `Tick` 改成下面这样：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向脚本绑定的 Actor；这段代码使它每秒沿 X 轴移动 120 cm。完整的生命周期、输入、碰撞和异步加载示例见 [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

## 示例与支持范围

| 要做什么 | 从这里开始 | 当前边界 |
| --- | --- | --- |
| Actor 生命周期、属性、输入和碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | Win64 Editor / Development |
| Timer、latent `await`、结束游戏时取消任务 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `Task<T>` 目前仅支持 `Task<int>`；`catch` / `finally` 内不能 `await` |
| C# 定义 Actor 和 Blueprint 可用成员 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 反射类型或签名改动后需重新编译并重启 Editor |
| Server RPC、属性复制和 `RepNotify` | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md)、[ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 有自动化测试；真实多人流程尚未验收 |
| UI 和存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 参见示例支持的 API |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 需要生成对应绑定 |

这是 C# 语言与 .NET API 的**受支持子集**，不能直接运行任意 .NET 程序或 NuGet 包。方法体热重载可用；标准 `CancellationToken` 的组合用法尚未进入默认 Build And Bind。Shipping、Android 和 iOS 尚未验收。具体差异见[语言执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)和[异步能力进度](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## 从源码构建与测试

从插件目录构建仓库内的 Actor 示例，并运行 C# Guest 测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

`Source/` 是 UE 插件模块，`Tools/` 是 C# 编译器，`Build/` 是构建脚本，`Samples/` 是示例。[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)和[迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)记录实现细节及未完成工作。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
