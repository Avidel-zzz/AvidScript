# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。使用 Roslyn 编译 C#，输出 WebAssembly，在 UE 中通过 Wasmtime / WAMR 运行。运行时不托管 CLR。

**开发中。** 当前开发环境为 UE 5.8 / Win64，仅支持部分 C# 语法和 .NET API。详见[已知限制](#limitations)。

[安装](#installation) · [用法](#usage) · [示例](#examples) · [已知限制](#limitations) · [开发](#development)

```csharp
// 每帧沿 X 轴移动，速度 120 cm/s
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor，`avid_on_tick` 是每帧回调。此片段可替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 方法。

<a id="quick-start"></a>
<a id="getting-started"></a>
<a id="installation"></a>

## 安装

依赖：

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

编译 Editor target，替换引擎路径和 `MyGame` 工程名：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

## 用法

1. 打开 Editor，在关卡中放置 Cube，设为 `Movable` 并选中。
2. 选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建完成后点击 **Play**，Cube 会移动、旋转和缩放。

修改 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 后，通过同一菜单重新构建。

<a id="samples"></a>
<a id="examples"></a>

## 示例

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 移动 Actor，处理输入和碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 用 `await` 等待定时器、取消任务 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 调用 Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，用 `RepNotify` 响应变化 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 调用项目 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 用 C# 声明 UE 类型、属性和函数，供蓝图使用 |

<details>
<summary>C# 定义 Actor：Projectile</summary>

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

生成并编译后，蓝图可继承 `Projectile`、读写 `LaunchSpeed`，通过纯函数节点调用 `GetLaunchSpeed()`。见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

</details>

<a id="当前边界"></a>
<a id="known-limitations"></a>
<a id="status"></a>
<a id="limitations"></a>

## 已知限制

- **C# / .NET**：不支持完整语言和类库，不能直接引用任意 NuGet 包。[语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)
- **异步**：泛型 Task 仅支持 `Task<int>`；`catch` / `finally` 内不能 `await`。[异步支持范围](Docs/Phase66/P66.C9_Async_Throw_Routing.md)
- **热重载**：支持修改方法体。新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。
- **验证范围**：Shipping、Android、iOS 和真实多人游戏的完整验证尚未完成。

<details>
<summary>编译器实验选项</summary>

以下用法需要编译器 API 或实验性参数，默认构建命令尚未全部支持：

| 用法 | 说明 |
| --- | --- |
| `static int Count = Initialize();` | [静态字段初始化、静态构造函数](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md) |
| 同步方法、属性访问器中的 `throw` | [部分用法需启用实验性参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build) |
| 在 `async` 方法中捕获同步异常 | [包括静态初始化异常](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md#windows-专项验证) |
| `target.Value = await ReadAsync();` | [普通 C# 对象的字段、属性](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证)；接口属性、生成的 UE 类型尚不支持 |

预览编译器仍有[预取消时序问题](Docs/Phase66/P66.C10_PreCancelled_Await_Contract.md)：token 已取消时，`finally` 可能延后一轮回调执行。

</details>

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

`Guest IR` 是编译器的中间表示；`ObjectHandle` 是运行时访问 UE 对象的句柄，脚本不持有裸 `UObject*`。

```text
Source/     UE 插件模块
Tools/      C# 编译器与代码生成工具
Build/      构建与测试脚本
Samples/    脚本示例
Docs/       设计文档与测试记录
```

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
