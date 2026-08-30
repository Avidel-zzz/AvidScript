# 验证策略

## 原则

- 测试规模跟随风险和影响面。实现期获取廉价反馈，阶段末集中运行昂贵 Gate。
- 构建、静态检查和 Automation 是机器证据；PIE、控制器、网络、视觉和真实游戏流程是独立人工验收，不能互相替代。
- 命令退出码为零不自动等于测试通过；必须核对测试宿主自报总数、通过数、失败数和结束标记。
- 未执行、被中断、日志丢失或候选 tree 已变化的结果不计为通过。

## 受控工具链

- Harness 与架构脚本使用 PowerShell 7 `pwsh -NoProfile`。
- .NET 从插件 cwd 使用 `%USERPROFILE%/.dotnet/dotnet.exe`，版本必须精确为 `8.0.416`。
- .NET 临时 `DOTNET_CLI_HOME`、NuGet cache 和 CLI state 放在仓库外的阶段目录。
- UE 使用 `C:\UnrealEngine` 的源码版 UE5.8；构建默认 no-clean、`-WaitMutex`、`-NoHotReloadFromIDE`。

## 分层 Profile

- `DocsOnly`：链接、JSON、入口大小、Harness audit；不启动 .NET 或 UE。
- `Managed`：受影响 C# console test runner；先读 csproj，AvidScript 自执行测试使用 `dotnet run`，不能用静默 `dotnet test` 代替。
- `Native`：架构检查和受影响 UE 模块 no-clean 构建。
- `Runtime`：Native 加聚焦 Automation；生命周期、Guest Memory、ObjectHandle、ABI 变化按高风险处理。
- `Performance`：冻结 workload、机器、配置、warmup、样本和统计口径后执行 benchmark；同时保留绝对值与对照组。
- `Full`：阶段最终 Gate，由 Phase workflow 预算控制。

`Auto` 根据 changed paths 和关键词保守选择 Profile；无法确定时扩大验证，不静默跳过。

## UE 证据

- Automation 使用受控启动模板和唯一 `-abslog`，验证完整测试计数、失败计数、队列完成与进程退出。
- 不在每个小批次重复启动 Editor。完整 AvidScript Automation 正常只在阶段 Gate 运行。
- 隔离 target 或候选构建之后，先恢复 canonical Editor target，再读取和比较 BuildId。
- clean architecture 证据必须来自干净候选；工作树探针只能作为开发反馈。

## 性能证据

- 性能目标是领先，不是只通过相对 Puerts/AngelScript 的门槛；同时记录纯执行、Host/Guest crossing、反射、对象属性和事件流程。
- 比较 JIT/AOT/runtime 时统一 workload、数据表示、优化级别和边界次数，区分引擎执行成本与 UE 反射交互成本。
- 基准结果必须记录 Git tree、runtime 版本、硬件、样本数、分位数和原始报告路径。
