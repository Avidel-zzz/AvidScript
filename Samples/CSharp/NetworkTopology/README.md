# C# 真实网络拓扑样例

该样例由 P57.12D5 独立进程编排器使用，覆盖一条完整的 UE 网络链路：

1. remote client 的 C# `BeginPlay` 安排零堆下一 Tick continuation，恢复后调用 generated
   `ServerSubmitValue(41)`；
2. UE callspace/NetDriver 把 Remote-only RPC 发送到 owning server Actor；
3. server 的 C# `before` handler 写入 `ReplicatedScore` 并记录 script side；
4. 原 Server RPC implementation 随后执行一次；
5. UE Push Model/replication 把属性送回 owning client；
6. client 原生 `OnRep_ReplicatedScore` 先执行，C# `after` handler 再记录并发送 confirmation RPC；
7. server 收到 confirmation 后，server/client 各自写出独立 JSON 结果。

样例同时把 `ServerSubmitValue` 放入 outbound `include_functions` 与 inbound `before_handlers`。这用于证明
客户端 Remote-only ProcessEvent 不会被本地 C# hook 误截获，而服务器 Local 入站调用会进入 handler。

运行入口：

```powershell
pwsh -File Plugins/AvidScript/Build/RunAvidScriptNetworkTopology.ps1
```

默认依次执行 dedicated server + 2 clients 与 listen server + 1 remote client。脚本只停止自己创建且仍
存活的精确 PID，不按进程名关闭用户 Editor。该样例是 Development Editor 验收，不代表 Cook、Shipping、
跨机器、丢包仿真、Iris replication graph 或移动端已完成。
