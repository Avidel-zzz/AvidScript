# C# 脚本定义 UE 类型样例

`ScriptDefinedTypes.cs` 展示 P59 的目标写法：

- `[UClass]` 声明 Actor、ActorComponent 和 WorldSubsystem；
- `[UProperty]` 声明 Blueprint 属性与 RepNotify；
- `[UFunction]` 声明 Blueprint 可调用函数；
- C# 类型继承和 `override` 进入同一稳定声明合同。

P59.A 已能由 Roslyn 生成并验证 `ue_type_declarations`，但尚未生成真实 native `UClass`。
需要等 P59.B 的 UHT shell generator 完成后，才能在 Editor 中放置 Actor 或创建 Blueprint 子类；
本说明明确区分已实现语义合同与后续 Runtime 能力。
