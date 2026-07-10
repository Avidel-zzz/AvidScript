# AvidScript C# Gameplay Authoring 指南

> 当前状态: 以 PC / Windows Editor 为主, 可以用受限 C# 子集生成 direct ABI WASM, 绑定到选中的 UE Actor, 并接入 `BeginPlay` / `Tick` / `EndPlay` 生命周期。Phase25 后, C# 已支持 `Actor.SetLocation(...)`、`Actor.AddLocationOffset(...)`、`private static float` 状态字段、字段赋值和字段累加; `.avid` 不是当前必经入口。

## 你现在能实现什么

当前 C# 路线已经可以完成一个状态化 gameplay loop:

1. 在 C# 里写 `BeginPlay()`、`Tick(float deltaSeconds)` 和 `EndPlay()`。
2. 用 `private static float` 保存脚本状态, 例如累计时间、速度或简单阶段值。
3. 在 `BeginPlay()` 中初始化状态, 并调用 `Actor.SetLocation(float x, float y, float z)` 设置初始位置。
4. 在 `Tick(float deltaSeconds)` 中使用 `Field += deltaSeconds` 这类语句累计状态。
5. 在 Actor 调用参数中使用数字、`deltaSeconds`、字段、加法和乘法表达式。
6. 在 `EndPlay()` 中执行脚本清理或最终状态写回。
7. 从 Editor 生成默认 C# profile, 构建 C# -> WASM/report/manifest, 并绑定到选中 Actor。
8. PIE/运行时由 `UAvidScriptComponent` 映射 UE BeginPlay/Tick/EndPlay 到三个 guest 导出。

这意味着你已经可以先写“Actor 初始化 + Tick 状态累积 + Actor 状态更新 + EndPlay 清理”级别的游戏逻辑, 用 C# 验证脚本语言到 UE 事件流的闭环。

## 当前 C# 示例

默认样例位于:

```text
Plugins/AvidScript/Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs
```

核心写法如下:

```csharp
using System.Runtime.InteropServices;

namespace AvidScript;

public static class ActorLifecycleScript
{
    private static float ElapsedSeconds;

    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        ElapsedSeconds = 0.0f;
        Actor.SetLocation(100.0f, 200.0f, 300.0f);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        ElapsedSeconds += deltaSeconds;
        Actor.SetLocation(100.0f + 120.0f * ElapsedSeconds, 200.0f, 300.0f);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        Actor.SetLocation(0.0f, 0.0f, 0.0f);
    }
}
```

`BeginPlay()` 对应 UE 组件开始运行的时机; `Tick(float deltaSeconds)` 对应 UE 组件 Tick, 其中 `deltaSeconds` 由宿主传入; `EndPlay()` 在组件卸载 WASM 前调用。上面的示例会在 BeginPlay 时把 Actor 移动到 `(100, 200, 300)`, 连续两个 Tick 后移动到 `(104, 200, 300)`, 最后在 EndPlay 回到 `(0, 0, 0)`。

## 支持的 C# 子集

Phase25 后, C# source adapter 的当前子集为 `actor_lifecycle_v4`:

- 支持 `BeginPlay()`。
- 支持 `Tick(float deltaSeconds)`。
- 支持可选 `EndPlay()`；未声明时 adapter 仍生成兼容的空 EndPlay 导出。
- 支持 `private static float Field;`。
- 支持 `private static float Field = 1.0f;`。
- 支持 `Field = expression;`。
- 支持 `Field += expression;`。
- 支持 `Actor.SetLocation(float x, float y, float z)`。
- 支持 `Actor.AddLocationOffset(float x, float y, float z)`。
- 支持数字 float 字面量, 例如 `100.0f`。
- 支持在 `Tick` 中使用 `deltaSeconds`。
- 支持字段引用, 例如 `ElapsedSeconds`。
- 支持受支持表达式之间的乘法和加法, 例如 `100.0f + 120.0f * ElapsedSeconds`。

暂不支持:

- 任意 C# 语句、分支、循环、局部变量、类实例、字段类型除 `float` 以外的状态、集合、异步、反射。
- 任意 UE API 访问。
- 多 Actor / 多 UObject 绑定。
- 碰撞 sweep、物理移动语义、hit result 或 movement component。
- 输入事件、Timer、Overlap/Hit 等额外 UE 事件。
- 移动端构建验证。

当前路线仍然是先把 UE 事件流、WASM ABI、组件绑定、Editor 工作流和少量高价值 Actor API 跑通, 再逐步扩大 C# 可写能力。

## 默认 Editor 工作流

### 1. 生成默认 C# profile

在 UE Editor 中执行:

```text
Tools > AvidScript > Create Default C# Profile
```

默认生成或确认存在:

```text
Saved/AvidScriptCSharpProfiles/default.csharp-profile.json
```

默认 profile 指向插件内 ActorLifecycle C# 示例, 并使用独立产物目录:

```text
Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle
```

如果 profile 已经存在, 默认不会覆盖。你可以直接编辑这个 JSON 的 `source_path`, 指到自己的 C# 文件。

### 2. 选中关卡 Actor

在关卡中选中一个 Actor。后续绑定会给这个 Actor 创建或复用 `UAvidScriptComponent`, 并把生成的 `.avidscript.json` manifest 写进组件属性。

### 3. 构建并绑定 profile

执行:

```text
Tools > AvidScript > Build And Bind C# Profile Script
```

成功后会产生:

```text
*.csharp_adapter.wasm
*.csharp.report.json
*.avidscript.json
```

然后 Editor 会读取 report 中的 manifest 路径, 绑定到选中 Actor 的 `UAvidScriptComponent`。

## 手动验证方式

1. 生成默认 profile。
2. 选中一个 Actor。
3. 执行 `Build And Bind C# Profile Script`。
4. PIE 或运行关卡。
5. 观察 Actor 是否在 BeginPlay 后移动到初始位置, 并在 Tick 后持续沿 X 轴移动。
6. 如需确认生命周期调用, 查看 UE log 中的 `AvidScript component start` 与首次 `AvidScript component tick`。
7. 停止 PIE 后, 默认样例会在 EndPlay 将 Actor 位置归零；自动化测试已验证该回调在 runtime 卸载和 owner handle 释放前执行。

## 常见诊断

C# profile/template/build/bind 输出会统一成 title/body/details 形式, 并带 `Next action`。常见类别如下:

| 类别 | 含义 | 下一步 |
| --- | --- | --- |
| `profile_missing` | 默认或指定 profile 不存在 | 先执行 `Create Default C# Profile`, 或确认 profile 路径正确。 |
| `source_missing` | profile 中的 `source_path` 不存在 | 指向真实 C# 文件, 或重新生成默认 profile。 |
| `project_missing` | profile 中的 `project_path` 不存在 | 指向真实 `.csproj`, 或恢复默认 ActorLifecycle profile。 |
| `source_adapter_failed` | C# 源码超出当前受限子集 | 先改回 `actor_lifecycle_v4` 支持的生命周期、字段、赋值和 Actor 调用形式。 |
| `build_failed` | PowerShell 构建脚本返回失败 | 检查 C# 是否超出当前支持子集, 再看 stdout/stderr。 |
| `report_missing` | 构建脚本没有写出 report | 检查输出目录权限与构建日志。 |
| `selection_unavailable` | 构建成功, 但没有可绑定 Actor | 在关卡中选中 Actor 后重试。 |

## 重要边界

当前 PC Editor 路线已经能验证 C# -> WASM -> UE Actor 的 BeginPlay/Tick/EndPlay 生命周期闭环, 并支持初始化位置、Tick 增量位移、静态 float 状态和状态化位置计算。当前 `Actor` 仍是静态 facade，`FVector` 尚未成为 C# 值类型，也没有完整 UObject/组件引用系统；下一阶段优先实现类型化 `FVector`、Actor 引用包装和 Transform API，再扩展输入、碰撞/Overlap/Hit、Timer、错误定位以及移动端验证。
