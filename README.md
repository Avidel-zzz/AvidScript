# AvidScript

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-to%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# to WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor and Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。构建时用 Roslyn 将脚本编译为 WASM；运行时通过生成的 UE 绑定访问游戏对象，游戏进程不加载 CLR。

当前支持 **UE 5.8 源码版、Win64 Editor / Development**。项目仍在开发，平台和语言限制见[支持范围](#支持范围)。

![C# 到 WASM、UE Runtime 和 UObject 的执行路径](Docs/Assets/README/pipeline.svg)

## 代码

来自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick`。把脚本绑定到 Actor 后，每秒沿 X 轴移动 120 cm：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前绑定的 Actor。示例文件还包含 `BeginPlay`、输入、碰撞和异步资源加载。

## 快速开始

需要 UE 5.8 源码版 C++ 工程、Visual Studio 2022 UE C++ 工具链、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译工程的 Editor target。将 `YourGame` 和引擎路径换成自己的工程名和路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../YourGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  YourGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 打开 Editor，在关卡中放置并选中一个 **Movable** Cube。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**。Cube 会移动、旋转和缩放。

修改[脚本](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)后，停止 Play，重新执行 Build And Bind。

## 示例

| 场景 | 示例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer / latent `await`、EndPlay 取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| C# 声明 Actor、属性和 Blueprint 函数 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| Server RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 复制属性与 `RepNotify` | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 绑定项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

## 支持范围

- **语言：**支持已实现的 C# 语法和部分 .NET API，不提供通用 .NET 运行时或任意 NuGet 包。`Task<T>` 当前仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`。[语言实现计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)
- **编辑：**方法体可热重载；反射类型或签名变化需要重新编译并重启 Editor。
- **网络：**RPC、复制属性和 `RepNotify` 有示例及自动化测试；真实多人玩法仍待项目内验收。
- **平台：**主要验证 Win64 Editor / Development；Shipping、Android、iOS 尚未验收。

标准 `CancellationToken` 与其他异步能力的组合尚未进入默认 Build And Bind 流程。实现进度见[能力组合合同](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## 开发

在插件根目录构建默认示例、运行 C# Guest 测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

`Source/` 是 UE 模块，`Tools/` 是编译器，`Build/` 是构建脚本，`Samples/` 是示例。设计和后续工作见[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)、[迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
