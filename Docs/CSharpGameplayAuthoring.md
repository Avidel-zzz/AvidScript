# AvidScript C# Gameplay Authoring 指南

> 当前状态: 以 PC / Windows Editor 为主, 可以用受限 C# 子集生成 direct ABI WASM, 绑定到选中的 UE Actor, 并接入 `BeginPlay` / `Tick` 生命周期。Phase23 后, C# 已支持 `Actor.SetLocation(...)` 与 `Actor.AddLocationOffset(...)`; `.avid` 不是当前必经入口。

## 你现在能实现什么

当前 C# 路线已经可以完成一个最小 gameplay loop:

1. 在 C# 里写 `BeginPlay()` 和 `Tick(float deltaSeconds)`。
2. 在 `BeginPlay()` 中调用 `Actor.SetLocation(float x, float y, float z)` 设置初始位置。
3. 在 `Tick(float deltaSeconds)` 中调用 `Actor.AddLocationOffset(float x, float y, float z)` 做增量移动。
4. 从 Editor 生成默认 C# profile。
5. 选中关卡里的 Actor。
6. 从 Editor 构建 C# -> WASM/report/manifest, 并绑定到该 Actor 的 `UAvidScriptComponent`。
7. PIE/运行时由组件在 UE `BeginPlay` 时加载 WASM 并调用 `avid_on_begin_play`, 在每帧 Tick 时调用 `avid_on_tick(deltaSeconds)`。

这意味着你已经可以先写“Actor 初始化 + Tick 位移”级别的游戏逻辑, 用它验证 C# 脚本语言到 UE 事件流的完整闭环。

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
    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        Actor.SetLocation(100.0f, 200.0f, 300.0f);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        Actor.AddLocationOffset(120.0f * deltaSeconds, 0.0f, 0.0f);
    }
}
```

`BeginPlay()` 对应 UE 组件开始运行的时机; `Tick(float deltaSeconds)` 对应 UE 组件 Tick, 其中 `deltaSeconds` 由宿主传入。上面的示例会在 BeginPlay 时把 Actor 移动到 `(100, 200, 300)`, 然后每帧沿 X 轴按 `120 * deltaSeconds` 增量移动。

## 支持的 C# 子集

Phase23 后, C# source adapter 的当前子集为 `actor_lifecycle_v2`:

- 支持 `BeginPlay()`。
- 支持 `Tick(float deltaSeconds)`。
- 支持 `Actor.SetLocation(float x, float y, float z)`。
- 支持 `Actor.AddLocationOffset(float x, float y, float z)`。
- 支持数字 float 字面量, 例如 `100.0f`。
- 支持在 `Tick` 中使用 `deltaSeconds`。
- 支持 `deltaSeconds * 数字字面量`。
- 支持受支持表达式之间的加法, 例如 `100.0f + 120.0f * deltaSeconds`。

暂不支持:

- 任意 C# 语句、分支、循环、类实例、字段状态、集合、异步、反射。
- 任意 UE API 访问。
- 多 Actor / 多 UObject 绑定。
- 碰撞 sweep、物理移动语义、hit result 或 movement component。
- 输入事件、Timer、Overlap/Hit 等额外 UE 事件。
- 移动端构建验证。

当前的目标是先把 UE 事件流、WASM ABI、组件绑定、Editor 工作流和少量高价值 Actor API 跑通, 再逐步扩大 C# 可写能力。

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
5. 观察 Actor 是否在 BeginPlay 后移动到初始位置, 并在 Tick 后持续沿 X 轴增量移动。
6. 如需确认生命周期调用, 查看 UE log 中的 `AvidScript component start` 与首次 `AvidScript component tick`。

## 常见诊断

C# profile/template/build/bind 输出会统一成 title/body/details 形式, 并带 `Next action`。常见类别如下:

| 类别 | 含义 | 下一步 |
| --- | --- | --- |
| `profile_missing` | 默认或指定 profile 不存在 | 先执行 `Create Default C# Profile`, 或确认 profile 路径正确。 |
| `source_missing` | profile 中的 `source_path` 不存在 | 指向真实 C# 文件, 或重新生成默认 profile。 |
| `project_missing` | profile 中的 `project_path` 不存在 | 指向真实 `.csproj`, 或恢复默认 ActorLifecycle profile。 |
| `build_failed` | PowerShell 构建脚本返回失败 | 检查 C# 是否超出当前支持子集, 再看 stdout/stderr。 |
| `report_missing` | 构建脚本没有写出 report | 检查输出目录权限与构建日志。 |
| `selection_unavailable` | 构建成功, 但没有可绑定 Actor | 在关卡中选中 Actor 后重试。 |

## 重要边界

当前 PC Editor 路线已经能验证 C# -> WASM -> UE Actor 生命周期闭环, 并支持初始化位置与 Tick 增量位移, 但还不是完整产品化脚本语言体验。下一步应优先扩展 C# 可写能力与绑定面, 例如状态字段、输入、碰撞/Overlap/Hit、Timer、更多 Actor API、错误定位到源文件行列, 再进入打包和移动端验证。