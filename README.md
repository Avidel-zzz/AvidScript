# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript：Unreal Engine C# 脚本插件" width="760">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-to%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# to WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor and Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

在 Unreal Engine 中写 C# 游戏逻辑。AvidScript 用 Roslyn 编译 C#，生成 WASM，由 UE 内的 Wasmtime 或 WAMR 运行；游戏进程无需加载 CLR。

目前面向 **UE 5.8 源码版、Win64 Editor / Development**。功能和平台边界见[支持状态](#支持状态)。

## 看一眼代码

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 每秒将 Actor 沿 X 轴移动 120 cm：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前绑定的 Actor。完整示例还包括 `BeginPlay`、输入、碰撞和异步加载。

## 运行示例

需要 UE 5.8 源码版 C++ 工程、Visual Studio 2022 UE C++ 工具链、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。在**工程根目录**安装插件：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译工程的 Editor target。将 `MyGame` 和引擎路径替换为自己的工程名和路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 Editor，在关卡中放置并选中一个 **Movable** Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。Cube 应移动、旋转和缩放。修改脚本后，停止 Play 并再次执行 **Build And Bind**。

## 更多示例

| 想做什么 | 从这里开始 |
| --- | --- |
| Actor 生命周期、输入、碰撞 | [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer / latent `await`、销毁时取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| C# 定义 Actor、属性、Blueprint 函数 | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |
| Server RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 复制属性、`RepNotify` | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI、存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

异步等待可以绑定到 Actor 生命周期；取消逻辑见 [LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)：

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
```

声明可供 Blueprint 使用的类型和属性；完整代码见 [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs)：

```csharp
[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;
}
```

## 支持状态

| 范围 | 当前边界 |
| --- | --- |
| C# | 支持已实现的语法及部分 .NET API；不能直接使用任意 NuGet 包。[语言实现计划](Docs/Phase66/P66.C_Language_Execution_Plan.md) |
| 异步 | Timer / latent `await` 和 Actor 销毁取消已有示例；`Task<T>` 目前仅支持 `Task<int>`，`catch` / `finally` 内不能 `await`。 |
| UE 类型 | 方法体改动可热重载；反射类型或签名变化需要重新编译并重启 Editor。 |
| 网络 | RPC、复制属性和 `RepNotify` 有示例及自动化验证；真实多人玩法仍需项目内测试。 |
| 平台 | 主要验证 Win64 Editor / Development；Shipping、Android、iOS 尚未验收。 |

标准 `CancellationToken` 尚未接入默认构建入口。静态字段与 token 已能从同一份 C# 源码生成并验证 Guest IR，但还不能输出可供 UE 加载的 WASM。进度见[能力组合合同](Docs/Architecture/AvidScript_Composable_Capability_Contract.md)。

## 开发

在插件根目录构建示例、运行 C# Guest 测试：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 源码到 UE 对象的编译与运行流程](Docs/Assets/README/pipeline.svg)

`Source/` 是 UE 模块，`Tools/` 是编译器和生成工具，`Build/` 是构建入口，`Samples/` 是示例。更多设计与计划见[模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)和[迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## License

[MIT](LICENSE)。第三方许可证：[Wasmtime](Source/ThirdParty/Wasmtime/README.md)、[WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
