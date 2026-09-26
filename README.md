# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 的 C# 脚本插件。C# 源码编译为 WebAssembly，由 Wasmtime 或 WAMR 执行，UE 进程不加载 CLR。

项目处于 **Experimental** 阶段，当前开发与测试环境为 UE 5.8 / Win64（Editor、Development）。C# 和 .NET API 的支持范围见[已知限制](#known-limitations)。

[安装](#quick-start) · [示例](#samples) · [已知限制](#known-limitations) · [构建与测试](#development)

## 代码示例

C# 定义 Actor，向蓝图暴露属性和函数：

```csharp
using AvidScript;

namespace AvidScriptSamples;

[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public float LaunchSpeed { get; set; } = 1200.0f;

    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public async void SetLaunchSpeedNextTick(float speed)
    {
        await AvidContinuations.NextTickAsync();
        LaunchSpeed = speed;
    }
}
```

蓝图可继承 `Projectile`。调用 `SetLaunchSpeedNextTick(900.0f)` 后，`LaunchSpeed` 在下一帧变为 `900`。

[完整源码](Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs) · [构建说明](Samples/CSharp/ScriptDefinedTypes/README.md)

<a id="installation"></a>
<a id="quick-start"></a>

## 安装

依赖：

- Unreal Engine 5.8 源码版
- Visual Studio 2022 + UE C++ 开发工具
- PowerShell 7
- .NET SDK [8.0.416](global.json)

### 1. 获取插件

在已有 UE C++ 工程的根目录执行：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

### 2. 编译工程

在 `Plugins/AvidScript` 下执行，将 `MyGame` 和 `$ueRoot` 改为自己的工程名与引擎路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path '../../MyGame.uproject').Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  MyGameEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

<a id="usage"></a>

### 3. 运行示例

先运行仓库自带的 `ActorLifecycle`：

1. 打开工程，放置 Cube，设置 `Mobility = Movable`。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

将 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 的 `Tick` 替换为以下内容，Actor 会以 120 cm/s 沿 X 轴移动：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    // UE.Self：当前绑定的 Actor。
    UE.Self.AddActorWorldOffset(new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

保存后重新执行 **Build And Bind** 即可加载修改。

<a id="examples"></a>
<a id="samples"></a>

## 示例

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | `BeginPlay` / `Tick` / `EndPlay`、输入、碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | `async` / `await`、定时等待、取消 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | Server / Client / Multicast RPC |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 属性同步、客户端 `RepNotify` 回调 |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | UI 更新、存档读写 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | C# 定义 Actor / Component / Subsystem |

<a id="当前边界"></a>
<a id="limitations"></a>
<a id="known-limitations"></a>

## 已知限制

| 项目 | 当前限制 |
| --- | --- |
| C# | 支持普通类的实例字段初始化与构造函数链；暂不支持[静态对象字段](Docs/Phase66/P66.C10_Static_Object_Lifetime_Contract.md)。 |
| .NET | 仅支持部分 API，不能直接使用任意 NuGet 包。 |
| `async` / `await` | 带返回值的 Task 仅支持 `Task<int>`；不支持在 `catch` / `finally` 中 `await`。 |
| 异常 | 部分同步方法、getter/setter 支持显式 `throw`，需启用[实验性构建参数](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md#generated-task-build)；尚不支持同步异常向异步方法传播。 |
| 热重载 | 支持方法体修改；新增 UE 类型、属性、函数或修改反射签名，需要重新编译并重启 Editor。 |
| 平台与发布 | Shipping、Android、iOS 和真实多人游戏验收尚未完成。 |

[C# 支持记录](Docs/Phase66/P66.C_Language_Execution_Plan.md) · [Task 异常与取消](Docs/Phase66/P66.C9_Async_Throw_Routing.md)

<a id="development"></a>

## 构建与测试

以下命令在插件根目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

编译使用 Roslyn 分析 C#，经 Guest IR（编译器中间表示）生成 WASM；运行时通过 UE 对象句柄调用引擎 API。

![C# 到 UE 的编译与运行流程](Docs/Assets/README/pipeline.svg)

```text
Source/    UE 插件模块
Tools/     C# 编译器与代码生成工具
Build/     构建与测试脚本
Samples/   示例项目
Docs/      设计文档与测试记录
```

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
