# AvidScript

![Unreal Engine 5.8](https://img.shields.io/badge/Unreal%20Engine-5.8-313131?logo=unrealengine&logoColor=white)
![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white)
![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white)
[![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的实验性 C# 脚本插件。C# 在构建时编译为 WebAssembly；UE 运行时加载 WASM，通过生成的绑定调用引擎 API，不在游戏进程中运行 CLR。目前主要验证平台是 Win64 Editor / Development。

![C# 源码、WASM 和 UE Runtime 的关系](Docs/Assets/README/pipeline.svg)

## 安装与运行

需要 UE 5.8 源码版 C++ 项目、Visual Studio 2022 的 UE C++ 工具链、PowerShell 7，以及 [.NET SDK 8.0.416](global.json)。在项目根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

然后编译项目的 Editor target。以下命令中的 `YourGame` 是项目名：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../YourGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  YourGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

在 Editor 中：

1. 打开 **Tools → AvidScript → Create Project C# Gameplay Workspace**。项目里会生成 `Scripts/AvidScript/GameplayScript.cs`。
2. 选中关卡中 RootComponent 可移动的 Actor，在 **Tools → AvidScript** 执行 **Build And Bind Project C# Gameplay Script**。
3. 点击 **Play**。默认脚本会在 `BeginPlay` 修改缩放，在 `Tick` 旋转 Actor。

修改 `GameplayScript.cs` 后，停止 Play 并重新执行 Build And Bind。创建工作区的命令不会覆盖已编辑的脚本；生成文件的位置见[工作区说明](Docs/Phase44/P44.3_Project_CSharp_Gameplay_Workspace.md)。

## 最小代码示例

下面的方法可以替换生成的 `GameplayScript.cs` 中的 `Tick`，让绑定的 Actor 每秒沿 X 轴移动 120 cm：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前脚本绑定的 Actor；`FVector` 和 `GetActorLocation` 等 API 来自项目的 UE 绑定生成器。想先运行仓库自带示例，可以选中一个 **Movable** Cube，执行 **Build And Bind C# ActorLifecycle Script**，再点击 **Play**。源码是 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

## 更多示例

- [Actor 生命周期](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)：`BeginPlay`、`Tick`、输入、碰撞。
- [异步玩法](Samples/CSharp/LatentGameplay/README.md)：Timer、资源加载和 `await`；Actor 或 World 结束时取消等待。
- [脚本定义 UE 类型](Samples/CSharp/ScriptDefinedTypes/README.md)：C# Actor、Component 及 Blueprint 可见成员。
- [网络](Samples/CSharp/NetworkRpc/README.md)：Server RPC；属性复制与 `RepNotify` 见[另一示例](Samples/CSharp/ReplicatedProperty/README.md)。
- [UI 与存档](Samples/CSharp/UiSaveDemo/README.md)：Widget 调用与保存数据。
- [项目 C++ API](Samples/CSharp/TypedProjectApi/README.md)：为项目类型生成 C# 绑定。

## 当前限制

- 只支持已实现的 C# / .NET 子集；不能直接运行任意 NuGet 包。`Task<T>` 当前只支持 `Task<int>`，`catch` 和 `finally` 内不能 `await`。
- 方法体可以热重载；脚本定义类型的反射签名变化后需要重新编译并重启 Editor。标准 `CancellationToken` 的组合用法尚未进入默认 Build And Bind。
- 网络功能有自动化测试，真实多人流程尚未验收；Shipping、Android 和 iOS 也尚未验收。

具体支持范围和待完成项见[语言执行计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)与[异步能力合同](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## 从源码构建与测试

在插件目录执行：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

`Source/` 是 UE 插件模块，`Tools/` 是 C# 编译器，`Build/` 是构建脚本，`Samples/` 是示例。实现细节见[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)和[迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
