# Build/AgentHarness

该目录预留给 H2/H3 的可复用检查器与 Gate 执行模块。H1 的单文件入口位于 `Build/InvokeAgentHarness.ps1`，以便先稳定 CLI 与 manifest 合同。

后续模块必须保持：

- Phase 状态仍由 `Build/InvokePhaseWorkflow.ps1` 拥有。
- checker 只读取显式输入，输出结构化结果和稳定错误码。
- Gate 缓存键包含 Git tree、命令、runner 版本、输入指纹和工具链 identity。
- 高成本执行器与路由/审计代码分离，不把主入口扩展成大型规则引擎。
