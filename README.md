# AvidScript

![UE 5.8](https://img.shields.io/badge/Unreal_Engine-5.8-313131?logo=unrealengine&logoColor=white) ![C# to WASM](https://img.shields.io/badge/C%23-to_WASM-512BD4?logo=dotnet&logoColor=white) ![Win64](https://img.shields.io/badge/platform-Win64-0078D4?logo=windows&logoColor=white) [![MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

AvidScript 是 Unreal Engine 5.8 的 C# 脚本插件。构建时将支持的 C# 代码编译为 WebAssembly；运行时由 UE 插件加载 WASM，通过生成的绑定访问引擎 API。游戏进程不加载 CLR。

![C# 到 UE 的构建与运行流程](Docs/Assets/README/pipeline.svg)

## 运行样例

以下步骤使用模板工程 `AvidTPSTemplate.uproject`，命令均在 `Plugins/AvidScript` 目录执行。需要 UE 5.8 源码版、Win64、Visual Studio 2022（UE C++ 工作负载）、PowerShell 7 和 [global.json](global.json) 指定的 .NET SDK 8.0.416。

1. 安装 Wasmtime 依赖并编译 Editor：

   ```powershell
   pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install

   $env:UE_ROOT = "C:\Path\To\UnrealEngine"
   $uproject = (Resolve-Path ../../AvidTPSTemplate.uproject).Path
   & (Join-Path $env:UE_ROOT "Engine\Build\BatchFiles\Build.bat") `
     AvidTPSTemplateEditor Win64 Development "-Project=$uproject" `
     -WaitMutex -NoHotReloadFromIDE
   ```

2. 打开模板工程，在关卡里放置并选中一个设为 `Movable` 的 Cube。
3. 执行 `Tools > AvidScript > Build And Bind C# ActorLifecycle Script`，然后点击 Play。Cube 会沿 X 轴移动，同时旋转和放大。

只编译样例 WASM、暂不启动 Editor：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

Play 后没有变化时，检查 Cube 是否挂有 `AvidScript Component`，并在 Output Log 中搜索 `AvidScript`。

## 脚本代码

[ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) 中的 `Tick` 每帧读取 Actor 位置，再按 `deltaSeconds` 沿 X 轴移动（120 UE 单位/秒）：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector currentLocation = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是当前绑定的 Actor。完整文件还包含 BeginPlay、异步加载、输入和碰撞回调。

## 更多样例

| 场景 | 示例 |
| --- | --- |
| Actor 生命周期、变换、输入与碰撞 | [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) |
| 下一帧、计时器、异步加载 | [LatentGameplay](Samples/CSharp/LatentGameplay/README.md) |
| RPC、属性复制、RepNotify | [NetworkRpc](Samples/CSharp/NetworkRpc/README.md) · [ReplicatedProperty](Samples/CSharp/ReplicatedProperty/README.md) |
| UI 与存档 | [UiSaveDemo](Samples/CSharp/UiSaveDemo/README.md) |
| 调用项目 C++ API | [TypedProjectApi](Samples/CSharp/TypedProjectApi/README.md) |
| C# 声明 Actor、Component、Subsystem | [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md) |

修改脚本声明的 `UClass`、`UProperty` 或 `UFunction` 后，需要重新构建并重启 Editor；步骤见 [ScriptDefinedTypes](Samples/CSharp/ScriptDefinedTypes/README.md)。

## 当前限制

- 主要在 Win64 开发和验证。Editor 与打包样例有自动化测试；真实游戏流程、Android 和 iOS 尚未验收。
- C# 只支持本项目实现的语法和 API 子集，不能直接运行任意 .NET 项目或 NuGet 包。
- RPC、属性复制和 RepNotify 已通过独立进程测试；真实多人联机尚未验收。
- `try` / `catch` / `finally` 的受限写法目前只在[专用测试入口](Docs/Phase66/P66.C3_Language_Error_Channel_Contract.md)可用；普通脚本构建尚未启用。
- `Task<int>` 的直接调用与 `await` 已有[语义合同](Docs/Phase66/P66.C4_Task_Result_Contract.md)，Guest/WASM 执行和等待唤醒尚未接通。

## 开发

运行 Guest 编译器测试：

```powershell
dotnet run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release
```

UE 模块在 [Source/](Source/)，C# 工具链在 [Tools/](Tools/)，构建脚本在 [Build/](Build/)。开发约定见 [AGENTS.md](AGENTS.md)，设计和测试记录见 [Docs/](Docs/)。

## License

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
