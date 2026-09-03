# PickupRush

`PickupRushScript.cs` 用 C# 实现 20 秒计时、5 次收集、定时复活、胜负与重开逻辑。UE 侧只提供可视化
宿主和 Startup Scenario，游戏规则不进入 Runtime C++。

在配套 UE5.8 TopDown 模板中执行：

```powershell
pwsh -NoProfile -File Build/InvokeAvidScriptPickupRush.ps1 -Mode Editor
```

该命令发布当前 Reflection 绑定、编译并 AOT 生成 WASM、启动真实地图，再验证 BeginPlay、Tick、
5 次 Gameplay Event 和最终胜利状态。需要人工游玩时使用 `-Mode Play`；PC 打包验证使用
`-Mode BuildCookRun -Configuration Development` 或 `Shipping`。

打包入口会先发布同配置 Generated Type；Editor、Development 与 Shipping 的 5/5 事件及胜利状态报告均已通过。

自动报告不能替代键鼠/手柄、碰撞手感与画面表现验收。
