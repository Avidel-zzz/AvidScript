# C# 入站 RPC 与 RepNotify 样例

该样例展示 AvidScript 如何把 UE 网络事件直接交给 C# 游戏逻辑：

- `ServerSubmitValue` 到达服务器实例时调用 `HandleServerSubmitValue(int value)`。
- `ReplicatedScore` 在客户端触发 `OnRep_ReplicatedScore` 时调用 `HandleReplicatedScoreChanged()`。
- RPC handler 可以继续使用生成式属性访问；示例在 authority 侧写入 `ReplicatedScore`，由 UE replication driver 负责传输。

`include_handlers`、`before_handlers` 与 `after_handlers` 接受项目 `AActor` 或 `UActorComponent` 上声明的 native 或 Blueprint-bytecode RPC/RepNotify UFUNCTION。生成器会校验网络方向、可靠性、关联 RepNotify 属性、链式模式和参数 ABI，然后生成 `AvidEvents` 常量；C# 使用 `[AvidEvent(...)]` 声明 handler，不需要为每个项目 API 编写 C++ binding。

该 profile 把 `ServerSubmitValue` 配置为 `before`，C# 成功后再执行原 RPC 实现；把 `OnRep_ReplicatedScore` 配置为 `after`，原 RepNotify 先执行再进入 C#。`include_handlers` 继续表示 `replace`。guest 调用期间发生的同步重入会深拷贝到有界 FIFO，并在下一次 Session tick 分发。
