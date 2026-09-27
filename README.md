# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&amp;logoColor=white" alt="UE 5.8">
  <img src="https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&amp;logoColor=white" alt="C# → WASM">
  <img src="https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&amp;logoColor=white" alt="Win64 Editor / Development">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 源码编译为 `.wasm`，通过生成的绑定调用 UE API。运行时使用 Wasmtime / WAMR，不依赖 CLR。

**开发中** · UE 5.8 源码版 · Win64 Editor / Development · [已知限制](#limitations)

[安装](#installation) · [用法](#usage) · [示例](#examples) · [开发](#development) · [文档](#docs)

<a id="用法"></a>
<a id="usage"></a>

## 用法

每帧移动 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(
        new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor。这个 `Tick` 让它沿 X 轴以 120 cm/s 移动。

将 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 替换为上面的代码，按下面的步骤运行。

<a id="getting-started"></a>
<a id="quick-start"></a>

<a id="installation"></a>

## 安装

需要 UE 5.8 源码版、UE C++ 工程、Visual Studio 2022（UE C++ 工具链）、PowerShell 7 和 [.NET SDK 8.0.416](global.json)。

在工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
cd Plugins/AvidScript
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor，替换为自己的引擎路径、工程名和 Editor target：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="运行示例"></a>

### 运行

1. 打开 Editor，选中关卡里的 Cube，将 **Mobility** 设为 **Movable**。
2. 执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建成功后点击 **Play**。

修改此示例：停止 Play → 修改 `.cs` → Build And Bind → Play。

<a id="示例目录"></a>
<a id="samples"></a>
<a id="examples"></a>

## 示例

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 移动 Actor，处理输入和碰撞 |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 用 C# 定义 Actor，让蓝图继承并读写属性 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 用 `await` 等待定时器、加载资源 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 在客户端与服务器之间调用函数（RPC） |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，在客户端接收 `RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 更新 UI，保存和读取存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目里的 C++ API |

<details>
<summary>代码：用 C# 定义蓝图可继承的 Actor</summary>

```csharp
using AvidScript;

namespace AvidScriptSamples;

[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    [UFunction(BlueprintPure = true, Category = "Projectile")]
    public float GetLaunchSpeed()
    {
        return LaunchSpeed;
    }
}
```

蓝图可继承 `Projectile`，读写 `LaunchSpeed`，调用 `GetLaunchSpeed()`。新增或修改这些 UE 类型声明后，需要生成代码、重新编译并重启 Editor。构建步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="开发"></a>
<a id="development"></a>

## 开发

在插件根目录执行：

```powershell
# 编译 ActorLifecycle 示例
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

```text
Source/    UE 插件模块
Tools/     C# 编译器、代码生成器与测试
Build/     构建脚本
Samples/   示例源码
Docs/      文档
```

<a id="已知限制"></a>
<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 已知限制

以下针对默认构建；实验性编译选项的进度见[异步异常与取消](Docs/Phase66/P66.C10_Cancellation_Token_Identity_Contract.md)。

- **C# / .NET**：仅支持部分语法和标准库 API，不能直接使用任意 NuGet 包。
- **`async` / `await`**：泛型 Task 仅支持 `Task<int>`；不支持在 `catch` / `finally` 内 `await`。
- **异常与取消**：`async` 方法尚不支持读取 `catch (Exception error)` 中的 `error`，也不支持读取 `OperationCanceledException.CancellationToken`。
- **热重载**：支持修改方法体。新增 UE 类型、属性、函数或修改签名，需要重新编译并重启 Editor。
- **平台**：Shipping、Android、iOS 和完整多人游戏流程尚未完成验收。

<a id="docs"></a>

## 文档

![C# 编译为 WASM，再由 UE 运行时加载并访问 UObject](Docs/Assets/README/pipeline.svg)

- [模块架构](Docs/Architecture/AvidScript_Module_Architecture.md)
- [C# 支持范围与实现进度](Docs/Phase66/P66.C_Language_Execution_Plan.md)
- [开发路线图](Docs/Architecture/AvidScript_Iteration_Roadmap.md)

## License

[MIT](LICENSE)。第三方许可证见 [Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
