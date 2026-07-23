# Typed Project API C# 样例

这个样例验证 C# 脚本已经可以沿 UE 的真实对象类型图编写一段完整游戏逻辑，而不需要为项目类手写 host import。

运行闭环如下：

1. `BeginPlay` 从 `UE.Self` 取得强类型 `AAvidScriptTypedTestActor`，调用项目自定义 `UFUNCTION` `ApplyGameplayValue`。
2. `UE.SpawnActor(ProjectClasses.Projectile, FTransform.Identity)` 返回强类型投射物。自动化测试会把 Profile 中的原生测试类替换为一个临时 Blueprint 子类，证明最终生成的是具体 Blueprint。
3. `Tick` 先通过隐式操作符逐级上转为直接基类，再用 `TryCast` 逐级执行有检查的下转。每次直接上转完全在 guest 内完成，不增加 host import；每个 `TryCast` wrapper 只包含一次 `avid_object_type_is_a`。
4. 成功下转后调用投射物自定义 `UFUNCTION` `ActivateProjectile`；把 Self 尝试下转为投射物时会得到 `default`，不会伪造有效 handle。
5. 第二次 `Tick` 先清空 transient 句柄和计数，再通过 `UE.DestroyActor` 只销毁脚本创建的投射物。
6. 如果脚本在第二次 `Tick` 前停止，`EndPlay` 会用同一套直接上转只清理仍有效的投射物；正常 Tick 已完成清理时，`EndPlay` 不会重复 Destroy。

对象 wrapper 只声明“派生类型到直接基类”的隐式上转，以及“目标派生类型从直接基类”的静态 `TryCast`。例如：

```csharp
AAvidScriptTypedTestActor typedActor = projectile;
AActor actor = typedActor;

AAvidScriptTypedTestActor checkedActor =
    AAvidScriptTypedTestActor.TryCast(actor);
AAvidScriptTypedTestProjectile checkedProjectile =
    AAvidScriptTypedTestProjectile.TryCast(checkedActor);
```

这种逐级表达与 UE 反射继承图一一对应，避免 C# 连续用户自定义转换的限制，也让每次运行时类型检查的成本和目标 ordinal 清晰可见。

Profile 使用 schema v3 的 `self_class_path`、项目类函数选择和 class reference。构建仍复用 ActorLifecycle 的 .NET 8/WASI 工程与构建脚本，输出默认位于 `Saved/AvidScriptCSharpGuest/Profiles/profile_typed_project_api`。

对应 Automation 为 `AvidScript.Editor.CSharp.TypedProjectApi`。它会走 Profile 解析、反射 descriptor、typed facade、Roslyn semantic artifact、reachable Guest IR、WASM、授权、WAMR 实例化以及 Runtime BeginPlay/Tick/EndPlay。测试会精确检查 typed Self、typed Spawn 和 checked downcast wrapper 到 dynamic import 的调用边，并覆盖 mismatch、stale handle、提前停止、幂等清理和确定性 Blueprint 清理。
