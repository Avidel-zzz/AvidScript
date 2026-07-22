# 可玩拾取物样例

`PlayablePickupScript.cs` 是一份由 C# 编译到 WASM、并通过 AvidScript 接入 Unreal Engine 事件流的完整游戏逻辑样例。

## 行为

- `BeginPlay`：初次加载沿用关卡 Actor 状态；热重载迁移到“已拾取”状态时重建复活计时器。
- `Tick`：未被拾取时，以每秒 90 度绕 Yaw 轴旋转。
- `OnBeginOverlap`：仅接受带有 `Player` Actor Tag 的对象，随后隐藏拾取物并关闭碰撞。
- `OnTimer`：3 秒后恢复拾取物，允许再次拾取。
- 热重载：新脚本或 manifest 无效时，AvidScript 拒绝候选版本并保留旧运行时。

## 使用方式

1. 使用 `EngineGameplay` profile 构建 `PlayablePickupScript.cs`。
2. 在关卡中把拾取物 Actor 配置为可见、启用碰撞并能产生 overlap，再绑定生成的 `.avidscript.json` manifest，或使用 Editor 构建报告自动创建并绑定 `UAvidScriptComponent`。
3. 给玩家 Actor 添加 `Player` Actor Tag，并确保拾取物能够产生 Actor BeginOverlap 事件。

`Collected` 使用显式持久状态合同。成功热重载发生在复活等待期间时，Actor 保持隐藏和无碰撞，新 WASM 会重新创建计时器；失败热重载继续保留旧 WASM。样例只调用自动生成的 `AActor` facade 和公共运行时原语，没有为 `ActorHasTag`、旋转、隐藏或碰撞编写专用宿主胶水。
