# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 编译为 WebAssembly，由 Wasmtime / WAMR 执行，UE 进程不加载 CLR。

**开发预览版 · UE 5.8 · Windows x64**。仅支持部分 C# 和 .NET API，具体见[已知限制](#limitations)。

[安装](#installation) · [运行](#quick-start) · [示例](#examples) · [开发](#development) · [已知限制](#limitations)

在 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中替换 `Tick` 方法，让 Actor 沿 X 轴以 120 cm/s 移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是绑定脚本的 Actor，`avid_on_tick` 是每帧回调入口。

<a id="installation"></a>

## 安装

需要：

- Unreal Engine 5.8 源码版
- Visual Studio 2022 + UE C++ 工具链
- PowerShell 7
- [.NET SDK 8.0.416](global.json)

在 UE C++ 工程根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

编译项目的 Editor target。替换引擎路径、`MyGame.uproject` 和 `MyGameEditor`：

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

## 运行

1. 打开 Editor，在关卡中放置 Cube，设为 `Movable` 并选中。
2. 选择 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 构建完成后点击 **Play**。

仓库自带的脚本会移动、旋转和缩放 Cube。修改脚本后，停止 Play，通过同一菜单重新构建，再次 Play。

<a id="samples"></a>
<a id="examples"></a>

## 示例

### C# 定义 Actor

[ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) 示例节选：

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

生成并编译后，蓝图可以继承 `Projectile`。`LaunchSpeed` 是可读写属性，`GetLaunchSpeed()` 是返回该属性值的蓝图节点。此示例的生成与构建方式见其 README。

### 更多示例

| 示例 | 用法 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 移动 Actor，处理输入和碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 等待一段时间后修改 Actor，销毁时取消等待 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 调用 Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，通过 `RepNotify` 处理变化 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 更新 UI，保存和读取存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目的 C++ API |

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

Roslyn 解析 C#，编译器将中间表示 Guest IR 转为 WASM。UE 插件加载 WASM，通过 `ObjectHandle` 查找和访问 UE 对象。

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

## 已知限制

| 项目 | 当前限制 |
| --- | --- |
| C# / .NET | 不能直接引用任意 NuGet 包；[语言支持范围](Docs/Phase66/P66.C_Language_Execution_Plan.md)仍在补全 |
| `async` / `await` | 泛型 Task 仅支持 `Task<int>`；`catch`、`finally` 内不能 `await`。见[异步支持范围](Docs/Phase66/P66.C9_Async_Throw_Routing.md) |
| 热重载 | 支持修改方法体；新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor |
| 发布与平台 | Shipping、Android、iOS 和真实多人游戏的完整验证尚未完成 |

<details>
<summary>实验中的语言功能</summary>

这些功能仍需编译器 API 或额外构建参数，默认构建命令尚未全部支持：

| 写法 | 说明 |
| --- | --- |
| `static int Count = InitCount();` | [静态字段初始化与对象生命周期](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md) |
| `try { ... } catch (...) { ... }` | [同步异常的构建参数与支持范围](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build) |
| `target.Value = await GetValueAsync();` | [异步成员赋值](Docs/Phase66/P66.C10_Await_Member_Assignment_Contract.md#guest-接入与补充执行验证)，暂不支持接口属性和生成的 UE 类型 |

已取消 token 的执行时序与验证进度见[预取消 await](Docs/Phase66/P66.C10_PreCancelled_Await_Contract.md)。

</details>

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
