# Replicated Property

本样例展示 C# 使用普通属性语法读写项目 `UPROPERTY(Replicated)` 与 `UPROPERTY(ReplicatedUsing=...)`。setter 只允许 authority 执行，成功写入后由 Runtime 使用活动 FProperty 的 RepIndex 标记 Push Model dirty。

已有 native/Blueprint RepNotify 仍由 UE 在接收复制更新时调用；authority 本地写入不会伪造 OnRep。D2 尚未把入站 RepNotify 自动转发为 C# handler，也不使用 Tick 轮询模拟该语义。
