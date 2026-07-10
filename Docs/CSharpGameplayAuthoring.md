# AvidScript C# Gameplay Authoring 指南

> 当前状态: PC / Windows Editor 优先。C# 受限子集可以生成 direct ABI WASM，绑定到 UE Actor，并接入 `BeginPlay` / `Tick` / `EndPlay`。Phase28 已支持 `FVector`、`FRotator`、`AActor`、`UE.Self`、Actor 位置/旋转读写和基础三分量表达式。

## 现在可以实现什么

当前闭环支持：

1. 在 C# 中编写 `BeginPlay()`、`Tick(float deltaSeconds)` 和可选 `EndPlay()`。
2. 使用 `private static float` 保存累计时间、速度或简单阶段状态。
3. 通过 `UE.Self` 获取当前 `UAvidScriptComponent` 的 owner Actor。
4. 使用 `FVector` 读取、设置或增量更新 Actor 位置。
5. 使用 `FRotator` 读取和设置 Actor 旋转。
6. 从 Editor 创建 profile、构建 WASM/report/manifest，并绑定到选中 Actor。
7. 在 PIE 中执行真实 Actor 读写，并在 EndPlay 时先调用 guest 再卸载 runtime 和释放 owner handle。

这已经足以编写“初始化 Actor、逐帧读取并更新位置/朝向、维护简单状态、结束时清理”的 C# 游戏逻辑。

## 默认示例

源码位于：

```text
Plugins/AvidScript/Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs
```

核心写法：

```csharp
public static class ActorLifecycleScript
{
    public static void BeginPlay()
    {
        UE.Self.SetActorLocation(new FVector(100.0f, 200.0f, 300.0f));
        UE.Self.SetActorRotation(FRotator.Zero);
    }

    public static void Tick(float deltaSeconds)
    {
        FVector currentLocation = UE.Self.GetActorLocation();
        UE.Self.SetActorLocation(
            currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));

        FRotator currentRotation = UE.Self.GetActorRotation();
        UE.Self.SetActorRotation(
            currentRotation + new FRotator(0.0f, 90.0f * deltaSeconds, 0.0f));
    }

    public static void EndPlay()
    {
        UE.Self.SetActorLocation(FVector.Zero);
        UE.Self.SetActorRotation(FRotator.Zero);
    }
}
```

`BeginPlay` 初始化位置与旋转；每个 Tick 读取当前 Transform 分量，沿 X 移动并增加 Yaw；`EndPlay` 在 runtime 卸载前归零。

## 类型化 API

### FVector

`FVector` 是按 X/Y/Z 顺序布局的 C# 值类型：

```csharp
FVector current = UE.Self.GetActorLocation();
FVector next = current + new FVector(10.0f, 0.0f, 0.0f);
UE.Self.SetActorLocation(next);
UE.Self.AddActorWorldOffset(new FVector(1.0f, 0.0f, 0.0f));
```

### FRotator

`FRotator` 按 Pitch/Yaw/Roll 顺序布局：

```csharp
FRotator current = UE.Self.GetActorRotation();
FRotator next = current + new FRotator(0.0f, 45.0f, 0.0f);
UE.Self.SetActorRotation(next);
```

当前 adapter 对 `FVector` 和 `FRotator` 共用三分量 local/expression codegen，支持 `Zero`、从 typed getter 初始化局部值，以及局部值与同类型构造值相加。

### AActor 与 UE.Self

`AActor` 只保存 object registry 的 slot/generation，不保存 `AActor*` 或宿主地址。`UE.Self` 通过 host imports 获取当前 owner。runtime 会验证 owner handle；缺失、无效或 stale generation 都会 fail closed。

自动化测试会先占用 slot 1，再注册目标 Actor，证明 typed API 不依赖固定句柄。struct read 使用经过范围验证的 guest linear memory，guest 不接触宿主指针。

旧位置 facade 仍兼容：

```csharp
Actor.SetLocation(10.0f, 20.0f, 30.0f);
Actor.AddLocationOffset(1.0f, 0.0f, 0.0f);
```

新代码建议使用 `UE.Self`、`FVector` 和 `FRotator`。

## 当前 C# 子集

Phase28 的 source adapter subset 为 `actor_lifecycle_v7`：

- `BeginPlay()`、`Tick(float deltaSeconds)`、可选 `EndPlay()`。
- `private static float Field;` 和可选初始值。
- `Field = expression;`、`Field += expression;`。
- 数字、`deltaSeconds`、静态 float 字段、加法和乘法表达式。
- `UE.Self.GetActorLocation()`、`SetActorLocation(FVector)`、`AddActorWorldOffset(FVector)`。
- `UE.Self.GetActorRotation()`、`SetActorRotation(FRotator)`。
- `FVector.Zero`、`FRotator.Zero`。
- `FVector` / `FRotator` typed getter 局部值。
- 局部三分量值与同类型构造值相加。
- 旧 `Actor.SetLocation(...)` 与 `Actor.AddLocationOffset(...)`。

当前不支持：

- 通用局部变量赋值、减法、整值标量乘法、成员访问和任意函数组合。
- 任意 `UFUNCTION`、`UPROPERTY` 或动态反射调用。
- 任意 Actor/UObject 参数、Spawn、组件查找和组件类型。
- `FTransform`、scale、quaternion、sweep 和 hit result。
- 输入、Timer、Overlap、Hit、委托和异步任务。
- 分支、循环、集合和完整 C# 语义。
- Android/iOS 构建验证。

## Editor 工作流

1. 执行 `Tools > AvidScript > Create Default C# Profile`。
2. 在关卡中选中目标 Actor。
3. 执行 `Tools > AvidScript > Build And Bind C# Profile Script`。
4. 启动 PIE，观察 BeginPlay 和 Tick 行为。
5. 停止 PIE，验证 EndPlay 清理。

默认 profile：

```text
Saved/AvidScriptCSharpProfiles/default.csharp-profile.json
```

默认产物目录：

```text
Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle
```

产物包括 `*.csharp_adapter.wasm`、`*.csharp.report.json` 和 `*.avidscript.json`。

## 常见诊断

| 类别 | 含义 | 下一步 |
| --- | --- | --- |
| `profile_missing` | profile 不存在 | 先创建默认 C# profile，或检查路径。 |
| `source_missing` | `source_path` 不存在 | 指向真实 C# 文件。 |
| `source_adapter_failed` | 源码超出 v7 子集 | 检查 lifecycle、静态 float、typed locals、表达式和 Actor 调用形状。 |
| `missing_import` | WASM 要求的 host import 未注册 | 确认 Runtime 与 manifest 版本一致。 |
| `host_import_failed` | owner handle 无效、stale 或 Actor 读写失败 | 检查组件 owner、对象生命周期、write policy 和 UE 日志。 |
| `selection_unavailable` | 构建成功但没有可绑定 Actor | 在关卡中选中 Actor 后重试。 |

## 下一步能力

Phase29 将优先增加 `FTransform`/scale 与 SceneComponent 常用 API，并开始把手写 binding contract 过渡到 UE 反射元数据驱动的生成流程。后续再推进 Spawn、输入/Timer/碰撞事件、调试工具、PC packaging 和移动端。
