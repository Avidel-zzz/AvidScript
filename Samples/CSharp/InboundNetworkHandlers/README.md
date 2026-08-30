# C# 入站 RPC 与 RepNotify 样例

该样例展示 AvidScript 如何把 UE 网络事件直接交给 C# 游戏逻辑：

- `ServerSubmitValue` 到达服务器实例时调用 `HandleServerSubmitValue(int value)`。
- `ReplicatedScore` 在客户端触发 `OnRep_ReplicatedScore` 时调用 `HandleReplicatedScoreChanged()`。
- RPC handler 可以继续使用生成式属性访问；示例在 authority 侧写入 `ReplicatedScore`，由 UE replication driver 负责传输。

`include_handlers` 接受项目 `AActor` 或 `UActorComponent` 上声明的 native RPC/RepNotify UFUNCTION。生成器会校验网络方向、可靠性、关联 RepNotify 属性和参数 ABI，然后生成 `AvidEvents` 常量；C# 使用 `[AvidEvent(...)]` 声明 handler，不需要为每个项目 API 编写 C++ binding。

P57.12D3 首批采用 replace 语义：对象命中已激活的脚本 handler 后，不再执行原生 `_Implementation` 或原生 OnRep 函数。未生成对应 C# export、候选热重载失败或 Session 卸载后，UFunction 保持或恢复原生 thunk。
