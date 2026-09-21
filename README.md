<div align="center">

# AvidScript

**用 C# 为 Unreal Engine 编写玩法脚本。**

<p>
  <img alt="Unreal Engine 5.8" src="https://img.shields.io/badge/Unreal%20Engine-5.8-0E1128?logo=unrealengine&amp;logoColor=white">
  <img alt="C#" src="https://img.shields.io/badge/Language-C%23-512BD4?logo=dotnet&amp;logoColor=white">
  <img alt="WebAssembly" src="https://img.shields.io/badge/Target-WebAssembly-654FF0?logo=webassembly&amp;logoColor=white">
  <img alt="Windows x64" src="https://img.shields.io/badge/Platform-Windows%20x64-0078D4">
  <img alt="0.1.0 开发预览版" src="https://img.shields.io/badge/Status-0.1.0%20Preview-D29922">
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/License-MIT-26A269"></a>
</p>

[🚀 跑起来](#快速开始) · [🧩 看代码示例](#常见用法) · [🚧 当前限制](#当前边界) · [📚 更多文档](#进一步阅读)

</div>

---

你可以让 Actor 移动和响应碰撞、等待计时器或资源加载、处理 UI 与存档，也可以用 C# 声明供蓝图使用的 Actor、组件、属性和函数。

| 🎮 编写玩法 | ⚡ 快速迭代 | 🔗 接入 UE |
| :--- | :--- | :--- |
| 移动、碰撞、UI、存档与网络 | 修改方法体后热重载，减少 C++ 编译等待 | 调用项目 API，向蓝图暴露类、属性和函数 |

> [!NOTE]
> **0.1.0 开发预览版** · 主要验证环境为 **UE 5.8 源码版 + Windows x64**。已有可运行样例，C# 语言支持、调试体验和平台覆盖仍在完善，尚不适合作为完整 .NET 的替代品。

<a id="快速开始"></a>

## 🚀 快速开始

先运行仓库自带的 ActorLifecycle 样例：让一个方块移动、旋转并逐渐变大。下面的命令都在项目的 `Plugins/AvidScript` 目录执行。

### 1. 准备环境

需要 Windows 10/11 x64、UE 5.8 源码版、Visual Studio 2022 的 UE C++ 构建环境、PowerShell 7、Git 和 **.NET SDK 8.0.416**。

将本仓库放到一个 C++ UE 项目的 `Plugins/AvidScript` 目录。安装插件使用的脚本执行引擎 Wasmtime：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

然后构建项目的 Editor。将下面的引擎路径、项目路径和 `YourProjectEditor` 替换为自己的配置：

```powershell
$env:UE_ROOT = "C:\UnrealEngine"

& "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" `
  YourProjectEditor Win64 Development `
  "-Project=C:\Path\To\YourProject.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

### 2. 编译并挂到方块上

1. 打开 UE Editor，确认 AvidScript 插件已启用。
2. 在关卡中放置一个 Cube，将 Mobility 设为 **Movable**，并选中它。
3. 在 **Tools** 菜单的 **AvidScript** 分组中，执行 **Build And Bind C# ActorLifecycle Script**。
4. 构建成功后，方块上会添加或复用 **AvidScript Component**，并自动填入脚本文件。
5. 点击 **Play**：方块会先移动到样例设定的位置，然后持续移动、旋转和变大。

完整脚本在 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。如果没有变化，先检查 **Output Log** 中的 `AvidScript` 构建或加载错误，以及方块上的组件是否已绑定脚本。

也可以单独用命令行编译：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

成功时会输出 `result=direct_abi_built`。生成的脚本入口位于**项目目录**下：

```text
Saved/AvidScriptCSharpGuest/ActorLifecycle/actor_lifecycle.avidscript.json
```

这个 JSON 是脚本的加载清单，记录要加载的程序及其信息。手动绑定时，在 Actor 上添加 **AvidScript Component**，将 **Script Manifest File** 指向它；**Script Module** 保持未设置。命令行编译本身不会修改关卡。

<a id="常见用法"></a>

## 🧩 常见用法

下面是从仓库样例中提取或简化的片段，用来说明写法；完整文件还包含必要的声明和配置，请从对应样例开始修改。

### 每帧移动 Actor

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FVector position = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        position + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
}
```

`UE.Self` 是挂载脚本的 Actor；`FVector` 是 UE 的三维向量。这里每秒沿 X 轴移动 120 个 UE 单位，乘以 `deltaSeconds` 后速度不依赖帧率。方法上方的特性把它接到 UE 的每帧更新事件。

同一套入口还有 `BeginPlay`（开始运行）和 `EndPlay`（结束运行）。见[生命周期样例](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。

### 等待一段时间，再继续执行

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    await AvidContinuations.DelayAsync(0.25f);
    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}
```

效果是开始运行后等待 0.25 秒，再把 Actor 放大。`await` 等待期间游戏继续运行。脚本所属对象或 World 被销毁时，关联等待会被取消，避免随后再操作已销毁的对象。

还可以等待下一帧（`NextTickAsync()`）或资源加载（`AvidAssets.LoadObjectAsync(...)`）。文档中的 **continuation** 指“等待完成后继续执行的那段代码”；**Latent** 是 UE 对这类延迟完成操作的称呼。资源加载写法见[生命周期样例](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)，主动取消见[延迟操作样例](Samples/CSharp/LatentGameplay/README.md)。

### 用 C# 定义蓝图可用的 Actor

```csharp
using AvidScript;

[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(EditAnywhere = true, BlueprintReadWrite = true)]
    public float Damage { get; set; } = 25.0f;

    [UFunction(BlueprintCallable = true)]
    public void Activate(float damageScale)
    {
        Damage *= damageScale;
    }
}
```

生成并编译 UE 类型后，`Projectile` 可以作为蓝图的父类；`Damage` 可以在编辑器和蓝图中修改，`Activate` 可以作为蓝图函数调用。

`UClass`、`UProperty`、`UFunction` 分别标记“UE 能识别的类、属性、函数”。这类脚本需要生成对应的 UE 类型，不能只把 `.cs` 文件挂到组件上。见[脚本定义 UE 类型样例](Samples/CSharp/ScriptDefinedTypes/README.md)。

### 找一个接近你需求的完整样例

| 你想实现什么 | 从哪里开始 |
| --- | --- |
| 碰到道具后隐藏，3 秒后重新出现 | [可拾取道具](Samples/CSharp/PlayablePickup/README.md) |
| 20 秒内收集 5 次，判断胜负并重开 | [PickupRush 小游戏](Samples/CSharp/PickupRush/README.md) |
| 点击按钮加分，保存并在下次启动时读回 | [UI 与存档](Samples/CSharp/UiSaveDemo/README.md) |
| 客户端请求服务器执行操作 | [RPC：远程调用](Samples/CSharp/NetworkRpc/README.md) |
| 服务器修改数值，客户端同步并响应变化 | [属性复制与 RepNotify](Samples/CSharp/ReplicatedProperty/README.md) |
| 调用项目自己的 C++ 函数、生成蓝图 Actor | [项目 API](Samples/CSharp/TypedProjectApi/README.md) |

RPC 是跨网络请求另一端执行函数；属性复制是服务器把属性值同步给客户端；RepNotify 是收到属性变化后的回调。调用权限和对象归属仍遵循 UE 的网络规则。

<a id="当前边界"></a>

## 🚧 当前边界

选型时请按下面的实际限制判断。功能出现在样例中，不表示任意 C# 写法或任意 UE 类型都已支持。

| 范围 | 目前能做什么 | 对开发的影响 |
| --- | --- | --- |
| C# 语言 | 常用控制流、受支持的类和结构体、同步 lambda、局部函数、共享捕获变量 | 还不能直接迁入任意 .NET / NuGet 库；普通 C# 类继承与多态、泛型执行覆盖、完整异常系统仍不齐全 |
| 异步 | 计时器、下一帧、资源加载及受支持的 UE 异步操作 | 跨 `await` 保留捕获对象、实例异步方法还未完整接通；不能使用任意 awaiter |
| UE 类型与 API | 属性、函数、常用数学类型、文本，以及受支持的数组、Set（去重集合）、Map（键值表） | 先在配置中选择需要的 API，再生成 C# 调用接口；部分嵌套对象容器、软引用（按路径引用资源）和弱引用（不阻止对象回收）的便捷用法仍有限制 |
| 委托与事件 | 受支持的单播、多播及 `ref/out` 参数 | 通过显式绑定、订阅接口使用；尚不能普遍写成 C# 的 `button.Click += Handler` |
| 热重载 | 更新方法体，迁移受支持的持久状态，拒绝无效候选版本 | 改移动速度这类逻辑可重载；新增反射属性、修改函数签名需要重新编译 UE 并重启 Editor；不能回滚任意外部副作用 |
| 调试 | 错误定位、调用栈、受支持的断点 / 单步、只读变量查看和性能分析 | 还不是完整 C# 调试器，部分语言能力与调试插桩不能组合使用 |
| Windows 打包 | 已有 Development / Shipping 样例与打包验证 | 正式包必须发布脚本模块并设置模块 ID；不能直接沿用上面的临时 JSON 路径 |
| 移动端 | 已有 Android 预编译准备 | Android 真机和 iOS 尚未验收，目前优先完善 Windows |

更具体的语言缺口和下一步见[当前实施计划](Docs/Phase66/P66.1_Implementation_Plan.md)。自动化测试、实际玩家操作和长时间运行的验收分别记录，不能互相代替。

## ⚙️ 它如何运行

```mermaid
flowchart LR
    Code["✍️ 编写 C# 玩法"] --> Build["📦 编译为 WASM"]
    Build --> Run["⚡ 插件加载并执行"]
    Run --> Game["🎮 驱动 UE 游戏对象"]
    classDef author fill:#172554,stroke:#60a5fa,color:#eff6ff
    classDef compile fill:#2e1065,stroke:#a78bfa,color:#f5f3ff
    classDef runtime fill:#052e2b,stroke:#2dd4bf,color:#f0fdfa
    class Code author
    class Build compile
    class Run,Game runtime
```

构建工具把 C# 编译成 **WebAssembly（WASM）**，一种供脚本执行引擎运行的程序格式；UE 插件加载它，再把脚本中的调用转交给 UE。游戏运行时不加载完整的 .NET / CLR。

项目 API 通过配置选择后生成 C# 接口。例如，选择项目的 `ApplyGameplayValue` 函数后，脚本才能以对应的 C# 方法调用它。技术文档中的 **Profile** 是这份选择配置，**binding / facade** 是生成的连接信息和 C# 调用接口。

主要执行引擎为 Wasmtime，另有 WAMR 兼容后端。底层执行引擎的细节通常不需要进入玩法代码。

<a id="性能摘要"></a>

## 📊 性能摘要

已有冻结用例的性能对比，部分 UE 调用路径取得优势；纯计算等项目仍有未达目标的指标，目前没有证据证明整体领先 Puerts 或 Unreal AngelScript。具体条件与结果见[性能报告](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md)，开发体验差距见[框架成熟度评估](Docs/Phase66/P66_Developer_Leadership_Assessment.md)。

![历史基准：四类 UE 调用的耗时对比，绿色为 AvidScript，灰色为 Puerts Reflection，柱形越短越好](Docs/Assets/README/phase57-prepared-reflection-performance.svg)

*上图为 P57 历史基准，不是当前版本的完整性能排名。绿色表示 AvidScript，灰色表示 Puerts Reflection；数值越低，指定调用耗时越少。测试环境、采样条件与适用范围见[原始证据](Docs/Phase57/P57.11B1_Recursive_Fixed_Struct_Codec_Evidence.json)。*

<a id="进一步阅读"></a>

## 📚 进一步阅读

- **开发与调试：**[IDE 工作区与编辑器命令](Docs/Phase61/P61.D4c2_Editor_IDE_Commands.md)、[调试面板](Docs/Phase61/P61.C4b_Editor_Debugger_Panel.md)。
- **发布 Windows 游戏：**[UI 样例的打包流程](Docs/Phase64/P64.D_Packaged_UI.md)、[插件打包与安装](Docs/Phase65/P65.A_Deterministic_Release_And_Atomic_Install.md)。
- **了解当前研发进度：**[实施计划](Docs/Phase66/P66.1_Implementation_Plan.md)、[设计与历史验证记录](Docs/)。这些是研发文档，不是入门前置阅读。
- **参与开发：**[仓库工作规则](AGENTS.md)。

<a id="许可证"></a>

## 📄 许可证

AvidScript 原创代码使用 [MIT License](LICENSE)。Wasmtime 使用 Apache-2.0 WITH LLVM-exception；WAMR 保留上游许可。Unreal Engine 不包含在本仓库中。
