# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 将 C# 脚本编译为 WebAssembly，在 Unreal Engine 中运行。编译器基于 Roslyn；UE 端使用 Wasmtime / WAMR，不托管 CLR。

开发预览版，当前开发和测试以 **UE 5.8 / Windows x64** 为主。C# 和 .NET API 尚未完整支持，使用前请查看[限制](#limitations)。

[安装](#installation) · [快速开始](#quick-start) · [示例](#examples) · [开发](#development) · [限制](#limitations)

<a id="installation"></a>

## 安装

环境要求：

- Unreal Engine 5.8 源码版
- Visual Studio 2022，含 UE C++ 工具链
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译 Editor。将下面的引擎路径和 `MyGame` 替换为自己的项目：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>
<a id="getting-started"></a>
<a id="quick-start"></a>

## 快速开始

1. 打开 Editor，在关卡中放置 Cube，设为 `Movable` 并选中。
2. 选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建完成后点击 **Play**，Cube 会移动、旋转和缩放。

脚本位于 [Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。例如，将它的 `Tick` 方法替换为：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // 沿 X 轴移动，速度 120 cm/s
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor；`avid_on_tick` 对应每帧回调。修改后通过同一菜单重新构建，再点击 **Play** 查看效果。

<a id="samples"></a>
<a id="examples"></a>

## 示例

### 定义 Actor

以下节选自 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)：

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

生成并编译后，蓝图可继承 `Projectile`，读写 `LaunchSpeed`，调用 `GetLaunchSpeed()` 节点。

### 更多示例

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | Actor 移动、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `await` 定时器、任务取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、`RepNotify` |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 项目 C++ API 绑定 |

<a id="development"></a>

## 开发

在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

编译时，Roslyn 解析 C#，经中间表示（Guest IR）生成 WASM。运行时，脚本通过对象句柄（ObjectHandle）访问 UE 对象，不持有裸 `UObject*`。

```text
Source/     UE 插件模块
Tools/      C# 编译器与代码生成工具
Build/      构建与测试脚本
Samples/    脚本示例
Docs/       设计文档与测试记录
```

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 限制

- **语言**：仅支持部分 C# 和 .NET API，不能直接引用任意 NuGet 包。见[语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)。
- **异步**：泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`。见[异步支持范围](Docs/Phase66/P66.C9_Async_Throw_Routing.md)。
- **热重载**：支持修改方法体。新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。
- **平台**：Shipping、Android、iOS 和真实多人游戏的完整验证尚未完成。

[静态初始化](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)、[同步异常](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)和 [`target.Value = await ...`](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证) 仍需编译器 API 或实验性参数；默认构建命令尚未全部支持。后者暂不支持接口属性和生成的 UE 类型。

已知问题：token 已取消时，`finally` 可能延后一轮回调执行。见[预取消时序问题](Docs/Phase66/P66.C10_PreCancelled_Await_Contract.md)。

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
