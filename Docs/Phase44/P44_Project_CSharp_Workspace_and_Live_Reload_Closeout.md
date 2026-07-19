# Phase 44：项目 C# Workspace 与事务式 Live Reload 收尾报告

> 状态：已完成。验证环境为源码版 UE5.8、Win64 Editor Development。

## 1. 阶段结论

Phase 44 已经把此前的 C#+WASM 编译链推进成一条可实际编写 Actor 游戏逻辑的项目级闭环：

1. 在 UE 项目内创建并保留用户拥有的 C# workspace；
2. 由 Roslyn Frontend、Semantic、Guest IR 和 WASM Backend 构建脚本；
3. 从 reflection authorization package 自动切出脚本实际使用的 runtime imports；
4. 在 Editor 中把产物绑定到选中的 Actor；
5. 通过 `UAvidScriptComponent` 接入 `BeginPlay`、`Tick`、`EndPlay`、Timer、Event 和 Gameplay Event；
6. 在运行中事务式切换新 WASM，失败时保留旧 runtime 与 Tick。

当前交付已经不是“只能注册 WASM”的底层样例。开发者可以用 C# 编写、构建、绑定并运行一个最小但真实的 UE 游戏脚本。

## 2. 当前可以开发什么

默认 starter 脚本已经用 generated facade 验证以下行为：

- `BeginPlay` 把 Actor 缩放从 `(2,2,2)` 重置为 `(1,1,1)`；
- `Tick(0.5)` 读取 `FRotator`，按每秒 90 度更新 yaw，使其从 `10` 变为 `55`；
- `UE.Self` 表示当前 Component 所属 Actor 的 typed handle；
- `FVector`、`FRotator`、`AActor.GetActorRotation`、`AActor.SetActorRotation` 和 `AActor.SetActorScale3D` 来自 reflection binding generator；
- Timer、通用事件、输入/碰撞等 Gameplay Event 已有生命周期入口，可以继续承载游戏规则。

典型脚本入口如下：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static void BeginPlay()
{
    UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
}

[UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
public static void Tick(float deltaSeconds)
{
    FRotator rotation = UE.Self.GetActorRotation();
    UE.Self.SetActorRotation(
        new FRotator(rotation.Pitch, rotation.Yaw + 90.0f * deltaSeconds, rotation.Roll),
        false);
}
```

## 3. 使用流程

1. 在 UE Editor 打开 `Tools > AvidScript`。
2. 执行 `Create Project C# Gameplay Workspace`。
3. 在 IDE 中打开 `<Project>/Scripts/AvidScript/AvidScript.Gameplay.csproj`。
4. 修改 `GameplayScript.cs`。
5. 在关卡中选择目标 Actor。
6. 执行 `Build And Bind Project C# Gameplay Script`。
7. 在运行中的世界观察脚本生命周期；已有 live Component 会尝试事务式 reload。

Build/Bind 菜单只编排既有 Workspace、Build 和 Binding service，不在 UI handler 中复制编译、切片或 runtime 逻辑。

## 4. 文件所有权

| 区域 | 路径 | 所有权与用途 |
| --- | --- | --- |
| 用户源码与工程 | `Scripts/AvidScript` | `GameplayScript.cs`、csproj、profile、`global.json`；默认 create-only，不覆盖用户修改 |
| IDE generated facade | `Intermediate/AvidScript/CSharpWorkspace` | 可刷新生成的 UE typed facade 与 binding package 投影 |
| 可加载产物 | `Saved/AvidScriptCSharpGuest/ProjectGameplay` | build report、manifest、WASM 与 runtime binding package |
| 语义缓存 | `Saved/AvidScript/CSharpSemanticCache/v1` | 内容寻址的 Semantic/Frontend 构建缓存 |

manifest 不依赖 `Intermediate` 中 generated facade 的绝对路径。用户源码、生成代码和可加载产物保持独立生命周期。

## 5. Cold/Warm 构建证据

同一份项目 Gameplay 脚本已经分别通过 cold 与 semantic-cache warm 路径：

| 指标 | Cold | Warm |
| --- | ---: | ---: |
| Build / Frontend / Semantic / Guest IR / WASM Backend | `2/1/1/2/2` | `2/0/0/2/2` |
| Authorization imports | 115 | 115 |
| Runtime package imports | 3 | 3 |
| WASM reflected imports | 3 | 3 |
| Component/WAMR BeginPlay | 通过 | 通过 |
| Component/WAMR Tick | 通过 | 通过 |

Warm build 只复用经 provenance 验证的语义产物，仍会重新执行 Guest IR、WASM backend、runtime slicing 与真实 WAMR 验收。

## 6. Live Reload 语义

对已经运行的 `UAvidScriptComponent`，候选 manifest 和 WASM 必须通过 hash、ABI、imports/exports、实例化与候选 `BeginPlay` 才能接管：

| 场景 | 结果 |
| --- | --- |
| 候选成功 | 提交新 runtime 与 manifest path，增加成功计数，新实例继续 Tick |
| manifest 无效 | 返回 `reload_rejected`，恢复旧配置 path，旧 runtime 继续 Tick |
| 候选 BeginPlay trap | 拒绝候选，旧 module、delegates 和 Tick 保持有效 |
| 原 Actor package 干净且 reload 失败 | 不新增 dirty 状态 |

C++ 可调用 `ReloadConfiguredScript(FAvidScriptWasmReloadResult&)` 获取完整诊断；Blueprint 可调用 `ReloadScript()` 获得 bool 结果。

## 7. 最终验证基线

P44.5 使用源码版 UE5.8 完成以下门禁：

| 验证项 | 结果 |
| --- | --- |
| .NET Frontend | 7/7 |
| .NET Semantic | 43/43 |
| Guest IR | 31/31 |
| C# Guest | 19/19 |
| WASM Backend | 11/11 |
| Semantic cache key / entry / prepared / integration | 16/16、25/25、11/11、8/8 |
| Build Integration / Publication | 11/11、4/4 |
| Parser | 18/18 |
| Architecture gate | 通过 |
| UE5.8 增量 UBT | 通过 |
| UE focused automation | Workspace 2/2、Module 8/8、Gameplay 1/1、Reload 1/1、Binding 3/3 |
| UE full `AvidScript` automation | 170/170，非成功 0 |

全量 UE automation 使用独立绝对日志、等待进程退出，并同时确认 `Automation Test Queue Empty` 与 `TestExit`。

## 8. 本阶段修正的构建确定性问题

同一个 content-addressed generated bindings 根目录中可能同时存在完整 authorization package 与只含少量 imports 的 runtime slice。测试或工具不能按最后修改时间选择“最新 package”，否则会把最小 runtime slice 当成完整编译授权。

P44.5 已把 BuildIntegration fixture 改为按所需 UE 函数能力筛选候选，并优先选择 import 覆盖更完整的 package。该修正不改变生产构建逻辑，但消除了测试对目录时间顺序的隐含依赖。

## 9. 尚未达到的成熟能力

当前闭环可以开始写游戏逻辑，但还不能宣称达到 Puerts、UnLua 或成熟 Angelscript 集成的完整体验：

1. reload 不迁移 guest 静态字段、managed heap、协程 continuation、Timer/Event 队列；
2. 候选 `BeginPlay` 在 trap 前已经产生的 Actor host 写入不会自动回滚；
3. 仍需手动执行 Build And Bind 或 `ReloadScript`，没有文件监听、防抖、取消与并发构建协调；
4. generated UE API 已是通用 reflection pipeline，但当前 gameplay 授权面仍为 115 项，不等同于完整 UE API；
5. 尚无 source map、C# 调用栈映射、断点与 IDE 调试器；
6. Cook、staging、Shipping、Android 与 iOS 产物策略尚未完成验证；
7. 当前正式语言路径是 C#+WASM，`.avid` 前端暂不在交付范围。

## 10. P45 建议目标

P45 应优先把“可运行”提升为“高频可迭代”：

1. 文件监听、debounce、构建取消和单飞 reload coordinator；
2. 脚本 instance/type 模型与更自然的 C# lifecycle 基类或 attribute surface，减少显式 export 样板；
3. source map、guest/host 调用栈映射和 Editor 诊断定位；
4. 明确的状态迁移协议，以及候选生命周期 host 写入的 command buffer 或隔离策略；
5. Cook/staging/Shipping 与移动端 artifact policy。

其中 P45.1 的自动监听与 reload coordinator 预计需要约 2 到 4 小时完成首个可验证切片；完整 P45 仍需按调试、状态与打包子阶段继续推进。
