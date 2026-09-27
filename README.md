# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript: C# scripting for Unreal Engine" width="800">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# to WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor and Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，通过生成的绑定调用 UE API；UE 进程中不加载 CLR。运行时提供 Wasmtime 和 WAMR 后端。

**状态：开发中。** 当前主要面向 UE 5.8 源码版、Win64 Editor / Development；Shipping 和移动端尚未验收。

## 代码示例

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每帧把 Actor 沿 X 轴移动 `120 × deltaSeconds` 厘米：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 指向绑定了当前脚本的 Actor。完整文件还包含 `BeginPlay`、输入、碰撞和异步加载。

## 运行示例

需要 UE 5.8 源码版的 C++ 工程、Visual Studio 2022 UE C++ 工具链、PowerShell 7，以及 [global.json](global.json) 指定的 .NET SDK 8.0.416。

在 **UE 工程根目录**执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译工程的 Editor target。下面的工程名和引擎路径按本机实际值修改：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

打开 Editor，在关卡中选中 **Movable** 的 Cube，运行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**，然后点击 **Play**。修改脚本后，停止 Play，重新执行 **Build And Bind**。

## 更多示例

| 示例 | 入口 |
| --- | --- |
| Actor 生命周期、输入和碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| Timer / latent `await` 与销毁取消 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| C# 定义蓝图可用的 Actor、属性和函数 | [ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) |
| Server RPC | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) |
| 属性复制和 `RepNotify` | [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 项目 C++ API 的类型化 C# 绑定 | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |

例如，[LatentGameplay](Samples/CSharp/LatentGameplay/README.md) 在等待 UE latent 函数时绑定取消源：

```csharp
await UKismetSystemLibrary.DelayAsync(0.25f)
    .WithCancellation(LifetimeCancellation.Token);
```

[ScriptDefinedTypes.cs](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) 用属性标注将 C# 成员暴露给 UE：

```csharp
[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;
}
```

## 当前限制

- C# 支持的是项目实现的语法和部分 .NET API，不能直接使用任意 NuGet 包；具体范围见 [C# 实现进度](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- `Task<T>` 目前只支持 `Task<int>`；不能在 `catch` / `finally` 内 `await`。标准 `CancellationToken` 属性已有双 VM 专项验证，但尚未进入默认构建入口，见 [取消语义合同](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。
- C# 定义的 UE 类型在方法体变化时可热重载；反射类型或签名变化需要重新编译并重启 Editor。
- RPC、复制属性和 `RepNotify` 有样例与自动化验证；真实多人玩法仍需项目内测试。Shipping、Android 和 iOS 尚未验收。

## 开发

在插件根目录运行：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 经 Guest IR 编译为 WASM，并通过 UE Runtime 调用 UE API](Docs/Assets/README/pipeline.svg)

`Source/` 是 UE 模块，`Tools/` 是编译器与生成工具，`Build/` 是构建脚本，`Samples/` 是示例。实现细节见 [模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)；后续工作见 [迭代路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)。

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) 和 [WAMR](Source/ThirdParty/WAMR/README.md)；Unreal Engine 不包含在本仓库中。
