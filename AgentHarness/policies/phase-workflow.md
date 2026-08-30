# Phase 工作流策略

## 唯一状态机

- `Build/InvokePhaseWorkflow.ps1` 及 `Docs/Phase*/Phase*_State.json` 是 Phase 状态的唯一 owner。
- Agent Harness 只负责发现、路由、影响分析和调用，不维护第二份 Phase 状态。
- 恢复、压缩、断线或自动续跑后，先执行 `bootstrap`；它必须先发现最高未关闭 Phase 并读取 `status`。
- 禁止硬编码“恢复到 Phase N”，禁止发明 PhaseWorkflow 子命令。

## 生命周期

固定顺序为：

1. 架构冻结：明确目标、非目标、模块边界、公开 API、Schema、ABI、生命周期、平台和 Gate。
2. 实现批次：按完整能力和独立写集拆成 2 至 4 组，产品代码、必要测试和最小文档一起落盘。
3. 集成与冻结：停止扩功能，汇总 Diff 和债务。
4. 集中审查：只做一次完整代码与架构审查，一次列全 findings。
5. 集中修复：普通问题合并处理；只有新 Blocker/Critical 才触发额外最小探针。
6. 统一 Gate：静态检查、受影响 .NET、no-clean UBT、Automation、必要 benchmark。
7. 小节提交与发布：每个架构、功能、集中修复或 Gate 小节完成集中验证后，精确核对 staged path，立即提交并推送 `origin/main`；不等待整个 Phase 结束。
8. 中文收尾：证据、文档、Git tree 和验证提交保持一致。

单次测试、一次构建修复或微小文档调整不单独形成碎片提交，应归入其所属小节。提交前必须确认 protected dirty 为零 staged、检查隐私与不必要生成物；push 失败时保留本地提交并优先恢复远端同步。

人读 Gate 报告必须在候选冻结前定稿。`attest` 后、见证提交前必须预检
`Test-AvidScriptAttestationDiff` 允许路径；默认只允许 Phase state、closeout 和标准
`P<Phase>_Gate_Summary.json`，不得把其他人读报告带入 attestation 提交。

已关闭 Phase 的外置 close evidence 必须对后续合法提交稳定：见证提交必须是当前
HEAD 的祖先，证据 tree 必须匹配该历史见证提交；不得要求当前 HEAD 永久停留在关闭提交。

## 实现期风险分流

- Blocker：核心工具链或实现路径不可继续，立即处理。
- Critical：崩溃、数据破坏、安全、ABI、生命周期、并发或跨平台契约风险，立即处理并做最小探针。
- Important：行为错误或明显退化，但不破坏后续基础，进入集中修复。
- Normal：文档、诊断、样式和低风险边界，阶段关闭前集中处理或明确转移。

实现期不为每个小任务重复构建、Automation、benchmark、独立复审或再复审。廉价编译反馈、静态检查和高风险最小探针不受此限制。

## 并行与等待

- 架构和写集冻结后才并行；并行写任务使用独立 worktree/branch，或证明路径完全不重叠。
- 编码任务应尽快产生可审查 Diff；长时间只有开放式分析时，缩小任务或收回主线。
- 长 Gate 等待期间只做不会改变候选 tree 的文档、Diff 审计和下一阶段分析。
- 任何相关源码变化都会使旧 Gate 证据失效。

完整定义见 `Docs/Workflow/Phase_Development_Workflow_Design.md`。
