# C# 双向属性示例

这个示例展示 Phase 52 的最小游戏逻辑闭环：脚本在 `BeginPlay`、`Tick` 和 `EndPlay` 中像普通 C# 属性一样读写 UE 反射属性，同时继续使用 `FVector` 调用通用反射函数。

## 脚本行为

- `BeginPlay` 把 `CustomTimeDilation` 设置为 `1.0`，并把 Actor 放到初始高度；
- `Tick` 按累计时间更新 `CustomTimeDilation`，同时沿 X 轴移动 Actor；
- `EndPlay` 把时间膨胀恢复为 `1.0`。

脚本侧只需要写：

```csharp
UE.Self.CustomTimeDilation = 1.0f;
```

生成器负责把该赋值映射到 descriptor v8 的 `property_set` binding。运行时在加载阶段缓存 `FProperty` 或 BlueprintSetter 调用计划，热路径不按名字查找反射成员。

## 权限

`BidirectionalProperties.csharp-profile.json` 使用 schema v5，只授权：

- `AActor.K2_GetActorLocation`；
- `AActor.K2_SetActorLocation`；
- 可写属性 `AActor.CustomTimeDilation`。

未列入 profile 的属性和函数不会进入授权 binding package。
