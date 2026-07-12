# AvidScript C# Gameplay Authoring 指南

> 当前状态：PC / Windows Editor 优先。Phase 38 已完成 C# direct ABI WASM、`BeginPlay` / `Tick` / `EndPlay` / Timer、统一 Gameplay Event、typed Overlap / Hit / Input、`FVector`、`FRotator`、`FTransform` snapshot、handle-backed `AActor` / `UE.Self`，以及 Actor 与 RootComponent typed read/write。

## 现在可以实现什么

1. 在 C# 中编写 `BeginPlay()`、`Tick(float deltaSeconds)`、可选 `EndPlay()`、`OnTimer(...)` 和 `OnEvent(...)`。
2. 编写 `OnBeginOverlap(AActor, FVector)`、`OnEndOverlap(AActor, FVector)` 和 `OnHit(AActor, FVector)`。
3. 编写 `OnInput(InputEvent)`，读取 ActionId、TriggerEvent 和三分量 Value。
4. 使用 `private static float` 保存累计时间、速度或简单阶段状态。
5. 通过 `UE.Self` 获取当前 `UAvidScriptComponent` 的 owner Actor。
6. 读取、设置或增量更新 Actor 位置、旋转和 scale，并获取 `FTransform` snapshot。
7. 从 Editor 创建 profile、构建 WASM/report/manifest，并绑定到选中 Actor。
8. 用 `UE.SetTimer(...)` 延迟执行脚本逻辑。
9. 从 UE 调用 `DispatchScriptEvent(eventId, value)` 推送 legacy event。
10. 从 Enhanced Input、传统 Input、AI 或网络回放调用 `DispatchScriptInput(actionId, triggerEvent, value)`。
11. 由组件自动接入 owner Actor 的 BeginOverlap、EndOverlap 和 Hit delegates。
12. 在 PIE 中执行真实 Actor 读写，在 EndPlay 时先调用 guest，再卸载 runtime 和释放 owner handle。

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

public static void OnTimer(int callbackId, int timerHandle)
{
    UE.Self.AddActorWorldOffset(new FVector(0.0f, 0.0f, 50.0f));
}

public static void OnEvent(int eventId, float value)
{
    UE.Self.AddActorWorldOffset(new FVector(0.0f, value, 0.0f));
}

public static void OnBeginOverlap(AActor otherActor, FVector location)
{
    otherActor.SetActorLocation(location + new FVector(0.0f, 10.0f, 0.0f));
}

public static void OnHit(AActor otherActor, FVector normalImpulse)
{
    otherActor.AddActorWorldOffset(normalImpulse);
}

public static void OnInput(InputEvent input)
{
    UE.Self.SetActorLocation(
        input.Value + new FVector(input.ActionId, input.TriggerEvent, 0.0f));
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

当前 adapter v12 尚不支持 lifecycle 内的 `FTransform` local，也不提供可能部分成功的非原子 `SetActorTransform`。需要修改时分别调用位置、旋转和 scale API。

### AActor 与 UE.Self

`AActor` 只保存 object registry 的 slot/generation，不保存 `AActor*`。`UE.Self` 通过 host imports 获取当前 owner；missing、invalid 或 stale generation 均 fail closed。自动化测试会先占用 slot 1，再注册目标 Actor，证明 typed API 不依赖固定句柄。

struct read 使用经过范围验证的 guest linear memory，guest 不接触宿主指针。旧位置 facade 仍兼容：

```csharp
Actor.SetLocation(10.0f, 20.0f, 30.0f);
Actor.AddLocationOffset(1.0f, 0.0f, 0.0f);
```

## 当前 C# 子集

Phase 38 source adapter subset 为 `actor_lifecycle_v12`：

- `BeginPlay()`、`Tick(float deltaSeconds)`、可选 `EndPlay()` / `OnTimer(int, int)` / `OnEvent(int, float)`。
- 生成式 `OnBeginOverlap` / `OnEndOverlap` / `OnHit` / `OnInput`，共用唯一 `avid_on_gameplay_event` 导出。
- `InputEvent.ActionId`、`InputEvent.TriggerEvent` 与 `InputEvent.Value` 字段读取。
- `UE.SetTimer(delaySeconds, callbackId)` 与 `UE.CancelTimer(timerHandle)` host ABI。
- `OnEvent` 内可用的 float payload `value`。
- `private static float`、字段赋值/累加。
- 数字、`deltaSeconds`、静态 float 字段、加法和乘法。
- Actor location、rotation、scale typed getter/setter。
- `AddActorWorldOffset(FVector)`。
- `FVector.Zero`、`FRotator.Zero`。
- `FVector` / `FRotator` typed getter local 与同类型加法。
- `UE.Self.GetRootComponent().GetWorldLocation()`。
- `UE.Self.GetRootComponent().SetWorldLocation(FVector)`。
- 旧 `Actor.SetLocation(...)` / `Actor.AddLocationOffset(...)`。

### SceneComponent 示例

```csharp
FVector rootLocation = UE.Self.GetRootComponent().GetWorldLocation();
UE.Self.GetRootComponent().SetWorldLocation(
    rootLocation + new FVector(10.0f, 0.0f, 0.0f));
```

`USceneComponent` 与 `AActor` 一样只持有 slot/generation。Host 返回组件时向经过验证的 guest 线性内存写入 8 字节句柄；重复取得同一 RootComponent 会复用注册表句柄，不持续增加 slot。

当前不支持：

- `USceneComponent` 局部变量与任意 UObject 数据流。
- 通用局部变量赋值、减法、整值标量乘法和任意函数组合；成员访问目前只开放已声明的 typed getter 与 InputEvent 三个字段。
- lifecycle 内的 `FTransform` local 与原子 `SetActorTransform`。
- 任意 `UFUNCTION`、`UPROPERTY` 或运行时动态反射调用。
- Spawn、组件查找、Attach/Detach、相对 Transform 和组件创建。
- quaternion、sweep、完整 `FHitResult`、Overlap component payload、自动生成 Enhanced Input Action 名称/ID 资产和异步任务。
- Timer repeat/pause/resume，以及 source adapter 内保存返回 handle 的完整 `int` 状态语法。
- 基于 `eventId` 的 if/switch；当前受限 adapter 只能直接使用事件的 float `value`。
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
| `source_adapter_failed` | 源码超出 v12 子集 | 检查 lifecycle、typed locals、Event value、表达式和 Actor 调用。 |
| `missing_import` | host import 未注册 | 确认 Runtime 与 manifest 版本一致。 |
| `host_import_failed` | handle、write policy 或 UE 调用失败 | 检查 owner 生命周期和 UE 日志。 |
| `selection_unavailable` | 没有可绑定 Actor | 在关卡中选中 Actor。 |

## 下一步能力

Phase 39 起主线进入正式语言前端、语义模型和诊断；Phase 42 进入 UE Reflection Binding Generator。后续能力包括事件分支/整数状态、生成式 UE API 覆盖、Spawn、调试工具、PC packaging 和移动端验证，不再通过逐个手写 API 扩张。
