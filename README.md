# AvidScript

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white)
![C# to WASM](https://img.shields.io/badge/C%23-to%20WASM-512BD4?logo=csharp&logoColor=white)
![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white)
[![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# → WebAssembly 脚本插件。Roslyn 在构建时编译脚本；运行时通过生成的绑定调用 UE API，无需在游戏进程中加载 CLR。

已验证：**UE 5.8 源码版、Win64 Editor / Development**。其他环境见[限制](#限制)。

![C# 到 WASM、UE Runtime 和 UObject 的执行路径](Docs/Assets/README/pipeline.svg)

## 代码示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每秒把绑定的 Actor 沿 X 轴移动 120 cm：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前绑定的 Actor。[完整文件](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)还有 `BeginPlay`、输入、碰撞和异步资源加载。

## 安装

要求：UE 5.8 源码版 C++ 工程、Visual Studio 2022 UE C++ 工具链、PowerShell 7、[.NET SDK 8.0.416](global.json)。在工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor target。将引擎路径、工程名和 `.uproject` 文件名改为实际值：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../YourGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  YourGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

## 运行示例

在 Editor 中：

1. 放置并选中一个 Mobility 为 **Movable** 的 Cube。
2. 运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**；Cube 会移动、旋转和缩放。

改动[脚本](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)后，先停止 Play，再执行一次 Build And Bind。

## 其他示例

| 用途 | 代码与说明 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer / latent `await`、EndPlay 取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| C# 声明 Actor、属性和 Blueprint 函数 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| Server RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 复制属性与 `RepNotify` | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 绑定项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

## 限制

- **C# / .NET：**只支持已实现的语法和 API，不能直接运行任意 .NET 程序或 NuGet 包。[语言实现计划](Docs/Phase66/P66.C_Language_Execution_Plan.md)
- **异步：**`Task<T>` 目前只支持 `Task<int>`；`catch` / `finally` 内不能 `await`。标准 `CancellationToken` 的组合用法尚未进入默认 Build And Bind。[异步能力进度](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)
- **热重载：**方法体可以热重载；UE 反射类型、属性或函数签名变化后需重新编译并重启 Editor。
- **网络：**RPC、复制属性和 `RepNotify` 有示例及自动化测试；真实多人流程尚未验收。
- **平台：**已验证 Win64 Editor / Development；Shipping、Android、iOS 尚未验收。

## 开发

从插件根目录构建默认示例、运行 C# Guest 测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

`Source/` 是 UE 模块，`Tools/` 是 C# 编译器，`Build/` 是构建入口，`Samples/` 是可运行示例。更多设计细节见[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)和[迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
