# AvidScript C# Gameplay Authoring 指南

> 当前状态: PC / Windows Editor 优先。Phase29 已支持 C# direct ABI WASM、`BeginPlay` / `Tick` / `EndPlay`、`FVector`、`FRotator`、`FTransform` snapshot、handle-backed `AActor` / `UE.Self`，以及 Actor 位置、旋转、缩放 typed read/write。

## 现在可以实现什么

1. 在 C# 中编写 `BeginPlay()`、`Tick(float deltaSeconds)` 和可选 `EndPlay()`。
2. 使用 `private static float` 保存累计时间、速度或简单阶段状态。
3. 通过 `UE.Self` 获取当前 `UAvidScriptComponent` 的 owner Actor。
4. 读取、设置或增量更新 Actor 位置。
5. 读取和设置 Actor 旋转。
6. 读取和设置 Actor scale，并获取 `FTransform` snapshot。
7. 从 Editor 创建 profile、构建 WASM/report/manifest，并绑定到选中 Actor。
8. 在 PIE 中执行真实 Actor 读写，在 EndPlay 时先调用 guest，再卸载 runtime 和释放 owner handle。

## 默认示例

源码：

```text
Plugins/AvidScript/Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs
```

核心写法：

```csharp
public static void BeginPlay()
{
    UE.Self.SetActorLocation(new FVector(100.0f, 200.0f, 300.0f));
    UE.Self.SetActorRotation(FRotator.Zero);
    UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
}

public static void Tick(float deltaSeconds)
{
    FVector location = UE.Self.GetActorLocation();
    UE.Self.SetActorLocation(
        location + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));

    FRotator rotation = UE.Self.GetActorRotation();
    UE.Self.SetActorRotation(
        rotation + new FRotator(0.0f, 90.0f * deltaSeconds, 0.0f));

    FVector scale = UE.Self.GetActorScale3D();
    UE.Self.SetActorScale3D(
        scale + new FVector(0.0f, 0.0f, 0.6f * deltaSeconds));
}
```

默认示例还会在 `EndPlay` 恢复位置、旋转和 scale。

## 类型化 API

### FVector 与 FRotator

`FVector` 按 X/Y/Z、`FRotator` 按 Pitch/Yaw/Roll 顺序布局。adapter 对两者共用三分量 local/expression codegen，支持 `Zero`、typed getter 局部值和同类型加法。

```csharp
FVector location = UE.Self.GetActorLocation();
FRotator rotation = UE.Self.GetActorRotation();
FVector scale = UE.Self.GetActorScale3D();

UE.Self.SetActorLocation(location + new FVector(10.0f, 0.0f, 0.0f));
UE.Self.SetActorRotation(rotation + new FRotator(0.0f, 45.0f, 0.0f));
UE.Self.SetActorScale3D(scale + new FVector(0.0f, 0.0f, 0.1f));
```

### FTransform snapshot

`FTransform` 投影组合 Translation、Rotation 和 Scale3D：

```csharp
FTransform snapshot = UE.Self.GetActorTransform();
```

当前 adapter v8 尚不支持 lifecycle 内的 `FTransform` local，也不提供可能部分成功的非原子 `SetActorTransform`。需要修改时分别调用位置、旋转和 scale API。

### AActor 与 UE.Self

`AActor` 只保存 object registry 的 slot/generation，不保存 `AActor*`。`UE.Self` 通过 host imports 获取当前 owner；missing、invalid 或 stale generation 均 fail closed。自动化测试会先占用 slot 1，再注册目标 Actor，证明 typed API 不依赖固定句柄。

struct read 使用经过范围验证的 guest linear memory，guest 不接触宿主指针。旧位置 facade 仍兼容：

```csharp
Actor.SetLocation(10.0f, 20.0f, 30.0f);
Actor.AddLocationOffset(1.0f, 0.0f, 0.0f);
```

## 当前 C# 子集

Phase29 source adapter subset 为 `actor_lifecycle_v8`：

- `BeginPlay()`、`Tick(float deltaSeconds)`、可选 `EndPlay()`。
- `private static float`、字段赋值/累加。
- 数字、`deltaSeconds`、静态 float 字段、加法和乘法。
- location、rotation、scale typed getter/setter。
- `AddActorWorldOffset(FVector)`。
- `FVector.Zero`、`FRotator.Zero`。
- `FVector` / `FRotator` typed getter local 与同类型加法。
- 旧 `Actor.SetLocation(...)` / `Actor.AddLocationOffset(...)`。

当前不支持：

- 通用局部变量赋值、减法、整值标量乘法、成员访问和任意函数组合。
- lifecycle 内的 `FTransform` local 与原子 `SetActorTransform`。
- 任意 `UFUNCTION`、`UPROPERTY` 或动态反射调用。
- UObject 参数/返回值、Spawn、组件查找和组件类型。
- quaternion、sweep、hit result、输入、Timer、Overlap、Hit、委托和异步任务。
- 分支、循环、集合和完整 C# 语义。
- Android/iOS 构建验证。

## Editor 工作流

1. 执行 `Tools > AvidScript > Create Default C# Profile`。
2. 在关卡中选中目标 Actor。
3. 执行 `Tools > AvidScript > Build And Bind C# Profile Script`。
4. 启动 PIE，观察 BeginPlay/Tick。
5. 停止 PIE，验证 EndPlay。

默认 profile：`Saved/AvidScriptCSharpProfiles/default.csharp-profile.json`。

默认产物目录：`Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle`。

## 常见诊断

| 类别 | 含义 | 下一步 |
| --- | --- | --- |
| `profile_missing` | profile 不存在 | 创建默认 profile 或检查路径。 |
| `source_missing` | source 不存在 | 指向真实 C# 文件。 |
| `source_adapter_failed` | 源码超出 v8 子集 | 检查 lifecycle、typed locals、表达式和 Actor 调用。 |
| `missing_import` | host import 未注册 | 确认 Runtime 与 manifest 版本一致。 |
| `host_import_failed` | handle、write policy 或 UE 调用失败 | 检查 owner 生命周期和 UE 日志。 |
| `selection_unavailable` | 没有可绑定 Actor | 在关卡中选中 Actor。 |

## 下一步能力

Phase30 将优先实现 SceneComponent/UObject 句柄返回和 typed component 调用，再把手写 binding contract 过渡到 UE 反射元数据驱动的生成流程。后续继续推进 Spawn、输入/Timer/碰撞事件、调试工具、PC packaging 和移动端。
