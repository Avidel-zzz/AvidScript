# AvidScript

<p align="center">
  <img src="Docs/Assets/README/avidscript-hero.svg" alt="AvidScript — C# scripting for Unreal Engine" width="800">
</p>

![UE 5.8](https://img.shields.io/badge/UE-5.8-313131?logo=unrealengine&logoColor=white) ![C# → WASM](https://img.shields.io/badge/C%23-%E2%86%92%20WASM-512BD4?logo=csharp&logoColor=white) ![Win64](https://img.shields.io/badge/Win64-Editor%20%2F%20Development-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Unreal Engine 的 C# 脚本插件。将 C# 编译为 WebAssembly，在 UE 中加载执行，不依赖 .NET CLR。

**Status:** 开发中，仅支持部分 C# 语法。当前测试环境为 UE 5.8 / Win64 Editor、Development，具体见 [Limitations](#limitations)。

[Quick start](#quick-start) · [Examples](#examples) · [Limitations](#limitations) · [Build & test](#build--test) · [License](#license)

<a id="usage"></a>

每帧沿 X 轴移动 Actor：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector offset = new FVector(120.0f * deltaSeconds, 0.0f, 0.0f);
    UE.Self.AddActorWorldOffset(offset);
}
```

`UE.Self` 是挂载脚本的 Actor，`avid_on_tick` 由 UE 每帧调用。这个函数以每秒 120 个 UE 单位移动 Actor。可替换 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick`，保留文件其余内容。

<a id="快速开始"></a>
<a id="安装"></a>
<a id="installation"></a>

## Quick start

### Requirements

- Unreal Engine 5.8 源码版
- Visual Studio 2022，已安装 UE C++ 开发工具
- PowerShell 7
- .NET SDK [8.0.416](global.json)

### Install

在工程根目录克隆插件并安装 Wasmtime 运行库：

```powershell
git clone https://github.com/Avidel-zzz/AvidScript.git Plugins/AvidScript
Set-Location Plugins/AvidScript

pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

构建 Editor target。以下使用 `AvidTPSTemplate`，换成自己的工程名和引擎路径：

```powershell
$ueRoot = 'C:\UnrealEngine'
$project = (Resolve-Path ../../AvidTPSTemplate.uproject).Path

& (Join-Path $ueRoot 'Engine\Build\BatchFiles\Build.bat') `
  AvidTPSTemplateEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

### Run

1. 打开工程，放置一个 Cube，将 **Mobility** 设为 **Movable**。
2. 选中 Cube，执行 **Tools → AvidScript → Build And Bind C# ActorLifecycle Script**。
3. 点击 **Play**，Cube 会移动、旋转和缩放。

编辑 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs` 后，再次执行第 2 步编译并绑定脚本。

<a id="示例"></a>

## Examples

| 示例 | 内容 |
| --- | --- |
| [ActorLifecycle](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | 移动 Actor，处理输入与碰撞 |
| [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) | 等待一段时间，取消未完成的等待 |
| [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) | 客户端、服务器之间调用函数（RPC） |
| [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) | 同步属性，在值变化时执行回调（RepNotify） |
| [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) | 更新 UI，读写存档 |
| [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) | 从 C# 调用项目中的 C++ API |
| [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) | 用 C# 声明 Actor、Component、Subsystem |

<details>
<summary>async / await 示例</summary>

等待 0.25 秒后缩放 Actor。如果 Actor 提前结束运行，`EndPlay` 会取消等待。

```csharp
private static AvidCancellationSource LifetimeCancellation;

[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    LifetimeCancellation = AvidCancellationSource.Create();

    await UKismetSystemLibrary.DelayAsync(0.25f)
        .WithCancellation(LifetimeCancellation.Token);

    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}

[UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
public static void EndPlay()
{
    LifetimeCancellation.Cancel();
    LifetimeCancellation.Release();
}
```

完整源码：[LatentGameplayScript.cs](Samples/CSharp/LatentGameplay/LatentGameplayScript.cs)。使用该示例的[构建配置](Samples/CSharp/LatentGameplay/README.md)。

</details>

<a id="当前边界"></a>
<a id="已知限制"></a>
<a id="支持范围"></a>

## Limitations

- C# / .NET：支持[部分 C# 语法](Docs/Phase66/P66.C_Language_Execution_Plan.md)，不能直接运行任意 .NET 程序或 NuGet 包。
- `async / await`：不支持在 `catch`、`finally` 中 `await`。异步异常处理需[显式启用](Docs/Phase66/P66.C6_Direct_Continuation_Await_Contract.md)；取消行为见 [Task 取消文档](Docs/Phase66/P66.C7_Async_Cancellation_Language_Contract.md)。
- Task 局部变量：重赋值和循环内声明尚不能编译为可运行的 WASM，见[实现进度](Docs/Phase66/P66.C8_Task_Local_Lifetime_Contract.md)。
- C# 声明的 UE 类型：修改 `UClass`、`UProperty`、`UFunction` 声明后，需要重新构建并重启 Editor。
- Android、iOS、Shipping 和真实多人游戏尚未验收。

<a id="构建与测试"></a>
<a id="开发"></a>
<a id="build-and-test"></a>
<a id="development"></a>

## Build & test

在插件目录执行：

```powershell
# 构建示例 WASM
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1

# 运行编译器测试
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

<a id="项目结构"></a>
<a id="internals"></a>

<details>
<summary>Repository layout</summary>

```text
Source/   UE 插件模块
Tools/    C# 编译器与 WASM 生成工具
Build/    构建与测试脚本
Samples/  示例
Docs/     文档
```

</details>

![C# 源码经编译器生成 WASM，再由 UE 运行时访问引擎对象](Docs/Assets/README/pipeline.svg)

编译时，Roslyn 分析 C# 代码，经中间表示（Guest IR）生成 WASM。运行时，脚本通过对象句柄（ObjectHandle）访问 UE 对象。

<a id="许可"></a>

## License

[MIT](LICENSE)

第三方依赖：[Wasmtime](Source/ThirdParty/Wasmtime/README.md) / [WAMR](Source/ThirdParty/WAMR/README.md)。Unreal Engine 不包含在本仓库中。
