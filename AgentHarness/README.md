# AvidScript Agent Harness

Agent Harness 是 `AGENTS.md` 与现有 Phase 状态机之间的轻量控制层。它负责启动检查、策略路由、历史教训检索、验证影响分析和自身完整性审计，不复制或替换 `InvokePhaseWorkflow.ps1`。

## 常用命令

```powershell
pwsh -NoProfile -File Build/InvokeAgentHarness.ps1 bootstrap -Intent "实现递归容器 binding"
pwsh -NoProfile -File Build/InvokeAgentHarness.ps1 context -Intent "优化 UFunction 调用" -Paths Source/AvidScriptBindings/Public/AvidScriptBindingInvocation.h
pwsh -NoProfile -File Build/InvokeAgentHarness.ps1 verify -Profile Auto -Paths Source/AvidScriptBindings/Public/AvidScriptBindingInvocation.h
pwsh -NoProfile -File Build/InvokeAgentHarness.ps1 lesson-query -Tags bindings,path
pwsh -NoProfile -File Build/InvokeAgentHarness.ps1 audit
```

所有命令都支持 `-Json`。`verify` 在 H1 只输出保守影响计划，不执行完整 Gate；缓存执行器和机器报告属于 H3。

## 目录职责

- `manifest.json`：策略注册、路径路由、工具链和大小限制。
- `policies/`：所有任务都必须遵守的仓库、Phase、安全和验证策略。
- `domains/`：按任务意图与修改路径加载的领域策略。
- `lessons/rules.json`：最多返回五条的通用教训索引。
- `lessons/legacy-agents-2026-08-30.md`：旧入口的无损原文，只定向检索。
- `schemas/`：Harness 结构化合同。

## 规则归属

- 可机械验证：实现为 Harness 检查。
- 稳定架构语义：写入对应 policy/domain 包。
- 可泛化事故：写入 `rules.json`，保留 legacy 来源。
- 日期流水或旧 Phase 细节：只进入历史库。
- 只有仓库级不可绕过边界才能修改根 `AGENTS.md`。
