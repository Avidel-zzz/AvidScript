# Agent Harness H0/H1 实施记录

> 日期：2026-08-30  
> 状态：H0、H1 已实现，H2、H3 待后续批次  
> 范围：代理规则系统；不修改 Phase 58 产品代码或 Phase 状态机

## 1. 背景

旧根 `AGENTS.md` 同时承担核心指令、架构说明、命令手册和事故历史，达到 580,219 字节、2,280 行。它远超适合作为常驻上下文的体量，最新规则存在无法稳定加载的风险。

本批次采用“薄 `AGENTS.md` + 可执行 Harness + 按需策略包 + 可检索历史库”。现有 `InvokePhaseWorkflow.ps1` 继续作为唯一 Phase 状态机，Harness 不建立第二份状态。

## 2. H0 无损归档

旧入口逐字节保存为：

```text
AgentHarness/lessons/legacy-agents-2026-08-30.md
```

归档身份：

| 指标 | 值 |
| --- | --- |
| 字节数 | 580,219 |
| 行数 | 2,280 |
| SHA-256 | `d72aa46f8ed3d298e4eb3de9b681c04083824bf15ce0446e33e5fdfa68e5dfe0` |

`manifest.json`、`rules.json` 和 `audit` 都保存或验证该 identity，确保原始知识可追溯。

## 3. H1 薄入口与路由

根 `AGENTS.md` 已缩减为 8,863 字节、115 行，只保留：

- 强制 `bootstrap` 启动协议。
- Git 根、UE5.8、PowerShell 7、.NET 8.0.416 与工作树安全边界。
- WASM-first、C#-first、backend 隔离、ObjectHandle、fail-closed、Session 生命周期和生成式 UE API 原则。
- 固定 Phase 生命周期、集中审查、集中 Gate 与中文文档要求。
- Harness、策略包和历史库路由。

四个核心策略包始终返回：

- `repository.md`
- `phase-workflow.md`
- `safety.md`
- `verification.md`

六个 Domain 包根据任务意图与路径命中：

- C# frontend
- Guest IR
- Bindings
- Runtime
- VM backends
- Performance

## 4. 可执行接口

`Build/InvokeAgentHarness.ps1` 已实现：

| 命令 | H1 行为 |
| --- | --- |
| `bootstrap` | 校验 Git 根和 PowerShell，自动发现最高未关闭 Phase，调用现有 `status`，统计 dirty，校验非 Phase-owned protected dirty，返回策略和最多三条教训 |
| `context` | 按 Intent、路径、关键词和 glob 返回四个核心包与命中的 Domain 包 |
| `verify` | 保守选择 `DocsOnly/Managed/Native/Runtime/Performance/Full`，只生成影响计划，不冒充 H3 Gate 执行 |
| `lesson-query` | 按 tag 查询通用教训，硬限制最多五条 |
| `audit` | 检查入口大小、bootstrap 链接、manifest Schema 有效性、策略注册、重复 ID、单入口和 legacy identity |

所有命令支持 `-Json`。稳定错误使用 `ASAH` 前缀。

## 5. 当前验证

本批次只验证 Harness，不启动 .NET、UBT 或 Unreal Editor：

```powershell
pwsh -NoProfile -File Build/InvokeAgentHarness.ps1 audit
pwsh -NoProfile -File Tests/AgentHarness/Test-AgentHarness.ps1
```

人工样例中，`bootstrap` 自动发现 `Phase 58 / implementing / P58.B`，protected dirty 内容为 `3/3` 稳定，自计时 1,179 ms，输出少于 100 行。最终数值以本批次合同测试输出为准。

## 6. 未实现范围

H2 将把高频错误继续代码化，包括路径索引、命令分隔符、SDK、Automation 模板、Phase 恢复和 dirty scope 检查。

H3 将实现 Gate Harness，包括变更影响图、缓存键、唯一日志、结果计数、可恢复检查点和机器可读报告。当前 `verify` 明确标记为 `impact-plan-only`，不会把计划误报为已执行证据。

## 7. Phase 58 隔离

本批次没有更新 `Docs/Phase58/Phase58_State.json`，没有完成或重开 `P58.B`，也没有修改产品模块。Phase 58 保持 `implementing`，产品工作树将在 Harness 重构验收后按原状态继续。
