![AvidScript：C# 代码驱动 Unreal Engine 游戏对象的概念插画](Docs/Assets/README/avidscript-hero.png)

<h1 align="center">AvidScript</h1>

<p align="center"><strong>用 C# 为 Unreal Engine 编写玩法脚本。</strong></p>

![UE 5.8 · C# · WebAssembly · Windows x64 · 0.1.0 Preview · MIT：带图标的本地徽章](Docs/Assets/README/project-badges.png)

<p align="center">
  <a href="#快速开始">🚀 快速开始</a> ·
  <a href="#常见用法">🧩 代码示例</a> ·
  <a href="#当前边界">🚧 当前限制</a> ·
  <a href="#进一步阅读">📚 更多文档</a>
</p>

---

用 C# 写 Actor 行为、异步流程和蓝图可用的类型。先看一个能运行的方块样例，再从下面的例子找自己的起点。

| 🎮 游戏玩法 | ⏱️ 等待与恢复 | 🧱 蓝图类型 |
| :--- | :--- | :--- |
| 方块每帧移动，碰撞后触发逻辑 | 等 0.25 秒再放大 Actor；对象销毁时取消等待 | 用 C# 声明 Actor、属性和可调用函数 |
| [看生命周期样例](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs) | [看延迟操作样例](Samples/CSharp/LatentGameplay/README.md) | [看脚本定义类型样例](Samples/CSharp/ScriptDefinedTypes/README.md) |

> [!NOTE]
> **0.1.0 开发预览版** · 主要验证环境为 **UE 5.8 源码版 + Windows x64**。已有可运行样例，C# 语言支持、调试体验和平台覆盖仍在完善，尚不适合作为完整 .NET 的替代品。

<a id="快速开始"></a>

## 🚀 快速开始

先运行仓库自带的 ActorLifecycle 样例：让一个方块移动、旋转并逐渐变大。下面的命令都在项目的 `Plugins/AvidScript` 目录执行。

![从 C# 脚本到 UE 游戏对象的四步流程：编写、编译、加载、运行](Docs/Assets/README/script-to-game.png)

### 🧰 1. 准备环境

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

### 🎮 2. 编译并挂到方块上

1. 打开 UE Editor，确认 AvidScript 插件已启用。
2. 在关卡中放置一个 Cube，将 Mobility 设为 **Movable**，并选中它。
3. 在 **Tools** 菜单的 **AvidScript** 分组中，执行 **Build And Bind C# ActorLifecycle Script**。
4. 构建成功后，方块上会添加或复用 **AvidScript Component**，并自动填入脚本文件。
5. 点击 **Play**：方块会先移动到样例设定的位置，然后持续移动、旋转和变大。

完整脚本在 [ActorLifecycleScript.cs](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)。如果没有变化，先检查 **Output Log** 中的 `AvidScript` 构建或加载错误，以及方块上的组件是否已绑定脚本。

<details>
<summary>⌨️ 可选：命令行编译与手动绑定</summary>

也可以单独用命令行编译：

```powershell
pwsh -NoProfile -File Build/BuildCSharpActorLifecycle.ps1
```

成功时会输出 `result=direct_abi_built`。生成的脚本入口位于**项目目录**下：

```text
Saved/AvidScriptCSharpGuest/ActorLifecycle/actor_lifecycle.avidscript.json
```

这个 JSON 是脚本的加载清单，记录要加载的程序及其信息。手动绑定时，在 Actor 上添加 **AvidScript Component**，将 **Script Manifest File** 指向它；**Script Module** 保持未设置。命令行编译本身不会修改关卡。

</details>

<a id="常见用法"></a>

## 🧩 常见用法

![三种可运行的 C# 玩法入口：每帧驱动方块、等待后继续执行、定义蓝图可用的 Actor](Docs/Assets/README/three-ways-to-start.png)

下面的片段从仓库样例提取或简化；完整文件还包含必要的声明和配置，请从对应样例开始修改。

### 🧭 每帧移动 Actor

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

### ⏱️ 等待一段时间，再继续执行

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    await AvidContinuations.DelayAsync(0.25f);
    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}
```

![BeginPlay 等待 0.25 秒后继续执行并放大 Actor；对象或 World 销毁时取消](Docs/Assets/README/async-lifecycle.png)

效果是开始运行后等待 0.25 秒，再把 Actor 放大。`await` 等待期间游戏继续运行。脚本所属对象或 World 被销毁时，关联等待会被取消，避免随后再操作已销毁的对象。

还可以等待下一帧（`NextTickAsync()`）或资源加载（`AvidAssets.LoadObjectAsync(...)`）。文档中的 **continuation** 指“等待完成后继续执行的那段代码”；**Latent** 是 UE 对这类延迟完成操作的称呼。资源加载写法见[生命周期样例](Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs)，主动取消见[延迟操作样例](Samples/CSharp/LatentGameplay/README.md)。

用 C# 新定义的 UE Actor 也可以在 `[UFunction] async void` 方法里等待下一帧：从蓝图或原生代码调用后，方法暂时返回，下一帧再修改该 Actor 的属性；对象在等待期间销毁则取消恢复。见[生成类型样例](Samples/CSharp/ScriptDefinedTypes/README.md)。

### 🧱 用 C# 定义蓝图可用的 Actor

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

### 🔔 订阅 UE 事件并记住得分

```csharp
int score = 40;
AvidSubscription subscription = AvidSubscriptions.SubscribeOnScriptSignal(
    UE.Self,
    (actor, amount, scale) => { score += amount; });

// 不再需要时：subscription.Cancel();
```

事件发生时，即使订阅函数已返回，回调仍会更新同一个 `score`。`OnScriptSignal` 是测试项目生成的事件名；在你的项目中，接口名称和参数由选中的 UE 事件决定。订阅代码应只执行一次，通常保存返回的句柄供取消使用。[完整写法与目前限制](Docs/Phase66/P66.B_Persistent_Event_State.md)。

对**已选中并生成绑定**的 UE 事件，也可以用熟悉的 C# 写法：

```csharp
public static class ScoreScript
{
    public static int Score;
    static void AddScore(AActor actor, int amount, float scale) => Score += amount;

    public static void Start() => UE.Self.OnScriptSignal += AddScore;
    public static void Stop()  => UE.Self.OnScriptSignal -= AddScore;
}
```

`Start()` 注册回调，`Stop()` 移除回调。重复注册会重复触发；同一脚本分别订阅两个有访问权限、同 World 的 Actor 时，移除其中一个不会影响另一个。

<details>
<summary>🔍 更多事件行为：再次广播、await、重载与取消</summary>

对这种已生成的无返回值多播事件，回调也能再次广播：例如先收到 `2`，再广播 `3`，两次回调会让得分共加 `5`。再次广播时要设置终止条件，避免无限递归。

事件回调也可以等待下一帧：收到 `2` 后先加 `2`，`await AvidContinuations.NextTickAsync()` 恢复后再加 `20`。等待期间的局部值会保留；同一份脚本作用于两个 Actor 时，停止或销毁其中一个只取消它自己的等待。这个流程已从 C# 自动生成的 `Projectile` 原生函数壳和 Win64 脚本包在 Editor 自动化中跑通；独立测试还覆盖 Wasmtime 与 WAMR 两后端。[可运行的碰撞事件样例](Samples/CSharp/ScriptDefinedTypes/README.md)和[重载、取消边界](Docs/Phase66/P66.B_Event_Language_Contract.md#事件回调跨-await)。

脚本卸载、World 清理或源 Actor 销毁后会自动解绑。更新脚本失败时旧回调继续工作，成功时旧回调解绑，新脚本可重新订阅。单播事件已被占用、对象已失效或对象属于另一个 World 时，运行时会拒绝新订阅。[完整的事件行为与当前限制](Docs/Phase66/P66.B_Event_Language_Contract.md)。

例如两个 Actor 的事件回调都在等待下一帧：包级更新失败时，两人的旧等待继续；更新成功时，两人的旧等待都取消，重新订阅后才能再触发。正式生成的 `Projectile` 已在 Win64 Editor 自动化中验证同包重载，以及把恢复后倍率从 `10` 改为 `20` 的不同代码版本切换；测试覆盖 Game 和 PIE 类型的 World，实际 Editor Play 操作、Cook/Shipping 切换和长时间运行仍待验收。[详细范围](Docs/Phase66/P66.B_Event_Language_Contract.md#事件回调跨-await)。

</details>

### 🗂️ 找一个接近你需求的完整样例

| 你想实现什么 | 从哪里开始 |
| --- | --- |
| 🎁 碰到道具后隐藏，3 秒后重新出现 | [可拾取道具](Samples/CSharp/PlayablePickup/README.md) |
| 🏁 20 秒内收集 5 次，判断胜负并重开 | [PickupRush 小游戏](Samples/CSharp/PickupRush/README.md) |
| 💾 点击按钮加分，保存并在下次启动时读回 | [UI 与存档](Samples/CSharp/UiSaveDemo/README.md) |
| 🌐 客户端请求服务器执行操作 | [RPC：远程调用](Samples/CSharp/NetworkRpc/README.md) |
| 🔄 服务器修改数值，客户端同步并响应变化 | [属性复制与 RepNotify](Samples/CSharp/ReplicatedProperty/README.md) |
| 🧱 调用项目自己的 C++ 函数、生成蓝图 Actor | [项目 API](Samples/CSharp/TypedProjectApi/README.md) |

RPC 是跨网络请求另一端执行函数；属性复制是服务器把属性值同步给客户端；RepNotify 是收到属性变化后的回调。调用权限和对象归属仍遵循 UE 的网络规则。

<a id="当前边界"></a>

## 🚧 当前边界

选型时请按下面的实际限制判断。功能出现在样例中，不表示任意 C# 写法或任意 UE 类型都已支持。

| 范围 | 目前能做什么 | 对开发的影响 |
| --- | --- | --- |
| C# 语言 | 常用控制流、受支持的类和结构体、同步 lambda、局部函数、共享捕获变量 | 还不能直接迁入任意 .NET / NuGet 库；普通 C# 类继承与多态、泛型执行覆盖、完整异常系统仍不齐全 |
| 异步 | 计时器、下一帧、资源加载；受支持的 `async void` 实例方法可跨 `await` 保留 `this`、参数、局部对象与共享变量 | 还不能等待任意 `Task` 或自定义 awaiter。[延迟加分示例与生命周期](Docs/Phase66/P66.B_Async_Invocation_Contract.md) |
| UE 类型与 API | 属性、函数、常用数学类型、文本，以及受支持的数组、Set（去重集合）、Map（键值表） | 先在配置中选择需要的 API，再生成 C# 调用接口；部分嵌套对象容器、软引用（按路径引用资源）和弱引用（不阻止对象回收）的便捷用法仍有限制 |
| 委托与事件 | 受支持的单播、多播及 `ref/out` 参数；显式订阅可接收捕获变量的 lambda、实例方法和静态方法；已生成的 UE 事件可用 `+=` / `-=` | 普通 .NET 事件不会自动接入 UE；事件语法的复杂委托组合仍在验收。[显式订阅范围](Docs/Phase66/P66.B_Persistent_Event_State.md)、[事件语法范围](Docs/Phase66/P66.B_Event_Language_Contract.md) |
| 热重载 | 更新方法体，迁移受支持的持久状态，拒绝无效候选版本 | 改移动速度这类逻辑可重载；新增反射属性、修改函数签名需要重新编译 UE 并重启 Editor；不能回滚任意外部副作用 |
| 调试 | 错误定位、调用栈、受支持的断点 / 单步、只读变量查看和性能分析 | 还不是完整 C# 调试器，部分语言能力与调试插桩不能组合使用 |
| Windows 打包 | 已有 Development / Shipping 样例与打包验证 | 正式包必须发布脚本模块并设置模块 ID；不能直接沿用上面的临时 JSON 路径 |
| 移动端 | 已有 Android 预编译准备 | Android 真机和 iOS 尚未验收，目前优先完善 Windows |

闭包、委托和 UE 事件的当前验收范围见 [P66.B 批次记录](Docs/Phase66/P66.B_Batch_Completion.md)；其他语言缺口和下一步见[当前实施计划](Docs/Phase66/P66.1_Implementation_Plan.md)。自动化测试、实际玩家操作和长时间运行的验收分别记录，不能互相代替。

## ⚙️ 它如何运行

构建工具把 C# 编译成 **WebAssembly（WASM）**，一种供脚本执行引擎运行的程序格式；UE 插件加载它，再把脚本中的调用转交给 UE。游戏运行时不加载完整的 .NET / CLR。

```mermaid
flowchart LR
    Source["✍️ C# 玩法代码"] --> Compiler["编译器"] --> Wasm["WASM 脚本"]
    UEAPI["🔎 从 UE 选出的 API"] --> Bindings["生成的 C# 调用接口"] --> Compiler
    Wasm --> Runtime["🧩 AvidScript 插件"] --> Game["🎮 UE Actor / UI / 蓝图"]
```

项目 API 通过配置选择后生成 C# 接口。例如，选择项目的 `ApplyGameplayValue` 函数后，脚本才能以对应的 C# 方法调用它。技术文档中的 **Profile** 是这份选择配置，**binding / facade** 是生成的连接信息和 C# 调用接口。

主要执行引擎为 Wasmtime，另有 WAMR 兼容后端。底层执行引擎的细节通常不需要进入玩法代码。

<a id="性能摘要"></a>

## 📊 性能摘要

已有冻结用例的性能对比，部分 UE 调用路径取得优势；纯计算等项目仍有未达目标的指标，目前没有证据证明整体领先 Puerts 或 Unreal AngelScript。具体条件与结果见[性能报告](Docs/Phase65/P65.D34_Production_Epoch_Runtime.md)，开发体验差距见[框架成熟度评估](Docs/Phase66/P66_Developer_Leadership_Assessment.md)。

![历史基准：四类 UE 调用的耗时对比，绿色为 AvidScript，灰色为 Puerts Reflection，柱形越短越好](Docs/Assets/README/phase57-prepared-reflection-performance.png)

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
