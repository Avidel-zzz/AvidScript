# AvidScript

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-to%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# to WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor and Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

AvidScript 将 C# 游戏脚本编译为 WASM，在 Unreal Engine 5.8 中运行。构建阶段使用 Roslyn；运行时通过生成的绑定调用 UE API，游戏进程不加载 CLR。

**Status:** Preview · UE 5.8 源码版 · Win64 Editor / Development。详见[支持范围](#支持范围)。

![C# 源码经 Roslyn 和 WASM 调用 UE 对象](Docs/Assets/README/pipeline.svg)

## 代码示例

以下代码来自 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。`UE.Self` 指向当前绑定的 Actor；`Tick` 每秒将它沿 X 轴移动 120 cm。

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

完整文件还包含 `BeginPlay`、输入、碰撞和异步资源加载。

## 安装与运行

需要 UE 5.8 源码版 C++ 工程、Visual Studio 2022 UE C++ 工具链、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在**工程根目录**安装插件：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译工程的 Editor target。把 `MyGame`、`.uproject` 文件名和引擎路径替换成自己的：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

启动 Editor，在关卡中放置并选中一个 **Movable** Cube。运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，再点击 **Play**。Cube 会移动、旋转和缩放。修改[脚本](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)后，停止 Play 并重新运行该菜单项。

## 更多示例

| 用途 | 示例 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| `await` 延迟、EndPlay 取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) |
| C# 声明 Actor、属性、Blueprint 函数 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) |
| Server RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 复制属性、`RepNotify` | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs) 在 `EndPlay` 取消等待：

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
```

[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 声明 UE / Blueprint 可见的 Actor 和属性（节选）：

```csharp
[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;
}
```

## 支持范围

| 范围 | 当前限制 |
| --- | --- |
| C# / .NET | 支持项目已实现的 C# 语法和部分 .NET API；不是通用 .NET 运行时，不能直接使用任意 NuGet 包。[语言计划](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| 异步 | 示例已覆盖 Timer / latent `await` 和 Actor 销毁取消；`Task<T>` 当前仅支持 `Task<int>`，`catch` / `finally` 内不能 `await`。 |
| UE 类型 | 方法体可热重载；反射类型或签名变化需重新编译并重启 Editor。 |
| 网络 | RPC、复制属性和 `RepNotify` 有示例及自动化测试；真实多人玩法仍需项目内验收。 |
| 平台 | 主要验证 Win64 Editor / Development；Shipping、Android、iOS 尚未验收。 |

标准 `CancellationToken` 尚未接入默认 Build And Bind 流程；异步能力组合仍在开发。进度见[能力组合合同](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## 开发

在插件根目录构建默认示例并运行 C# Guest 测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

源码目录：`Source/`（UE 模块）、`Tools/`（编译器）、`Build/`（构建脚本）、`Samples/`（示例）。设计文档见[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)和[迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
