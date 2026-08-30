# C# RPC 样例

该样例展示同一份 C# 游戏逻辑如何遵循 UE authority 方向发起 RPC：客户端调用 `ServerSubmitValue`，服务器调用 `ClientApplyValue` 与 `MulticastAnnounceValue`。

项目接入时，把 profile 中的测试 Actor 路径和函数名替换为自己的 `AActor`/`UActorComponent` UFUNCTION。AvidScript 不需要为新 RPC 增加手写 binding；重新生成 profile 即可得到强类型 C# facade。

D1 只覆盖脚本发起 RPC。脚本接收 RPC 与 Replicated Property/RepNotify 将在 P57.12D 后续批次完成。
