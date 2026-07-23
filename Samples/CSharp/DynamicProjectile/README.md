# DynamicProjectile C# 样例

该样例通过 `DynamicProjectile.csharp-profile.json` 选择所需的 Actor API，并声明项目蓝图类 `TwinStickProjectile`。Profile 复用 ActorLifecycle 的 .NET 项目与构建脚本，构建产物写入 `Saved/AvidScriptCSharpGuest/Profiles/profile_dynamic_projectile`。

运行时闭环如下：

1. `BeginPlay` 只注册一次性 Spawn Timer。
2. Timer 回调通过 `UE.SpawnActor` 创建 `BP_TwinStickProjectile`。
3. `Tick` 使用生成式 `GetActorLocation` 和 `SetActorScale3D` 更新投射物的统一缩放。
4. 第二个 Timer 通过 `UE.DestroyActor` 销毁投射物。

Actor handle、存活标记和累计时间均声明为 `AvidTransient`，不会跨 reload 迁移；候选激活后应从自身生命周期重新建立这些运行时资源。

`Tests/Fixtures/CSharp/P49_4_DynamicProjectileRejectedReload.cs` 是 reload 防御夹具。它在候选模块的 `BeginPlay` 中直接 Spawn，预期由 host-effect journal 以 `binding_reload_effect_unsupported` 拒绝，已激活的旧模块和世界对象不应因此被替换。
