# AvidScript Agent 入口

本文件是 AvidScript 仓库的常驻指令，只保存不可绕过的边界和路由协议。具体工作流、命令、领域规则与历史教训由 `AgentHarness` 按需提供，不要把日期日志或 Phase 细节追加回本文件。

## 强制启动

进入仓库、恢复任务、上下文压缩或网络重连后，第一个插件仓库命令必须是：

```powershell
pwsh -NoProfile -File Build/InvokeAgentHarness.ps1 bootstrap -Intent "<当前任务>"
```

要求：

- 使用 PowerShell 7。Harness 失败时先修复入口或报告 blocker，不绕过它继续写产品代码。
- 读取 bootstrap 返回的 Phase 状态、dirty/protected 状态、下一步和策略路径。
- 修改路径明确后执行 `context -Intent "<任务>" -Paths <paths>`，只读取返回的策略包。
- 遇到重复错误或历史不确定性时执行 `lesson-query -Tags <tags>`，每次最多读取 5 条。
- 结束前执行 `audit`；验证范围由 `verify -Profile Auto -Paths <paths>` 给出，不临场拼接完整 Gate。

## 仓库与工具链

- Git 根是当前 `Plugins/AvidScript`，不是 `AvidTPSTemplate` 工程根。Git 操作从插件根执行。
- 默认分支为 `main`。用户已持续授权：每个可独立审查的小节完成集中验证后，立即按精确路径白名单 commit 并 push 到 `origin/main`；不等待整个 Phase 结束。该授权不包含 rebase、reset、强推或改写历史。
- 使用源码版 Unreal Engine 5.8，引擎根固定为 `C:\UnrealEngine`。
- .NET host 为 `%USERPROFILE%\.dotnet\dotnet.exe`，仓库 SDK 固定为 `8.0.416`。从插件 cwd 验证版本后使用；缓存与 CLI state 放在仓库外。
- 文件发现先使用 `rg --files`，内容检索使用 `rg`。路径必须来自索引；Windows 通配符使用 `-g`，禁止猜路径。
- 一个 shell 调用只承担一个逻辑动作或证据域。禁止用 `;`、`&&`、`||` 串联独立动作；多读取使用并行工具或拆分调用。

完整规则：`AgentHarness/policies/repository.md`。

## 工作树安全

- 工作树可能同时含用户改动、当前 Phase 产品改动和 Harness 改动。默认全部保留，不回退、不覆盖、不顺手格式化无关文件。
- 当前 Phase state 中的 `protected_dirty` 是不可漂移基线。Harness 报告漂移时，停止触碰相关文件并说明差异。
- 禁止使用 `git reset --hard`、`git checkout --`、`git clean` 或等价破坏性命令，除非用户明确授权具体目标。
- 不清理 Editor target、`Binaries`、`Intermediate`、`Saved`、`DerivedDataCache` 或引擎输出来“尝试修好”构建。
- 不结束用户的 Editor、编译器或其他进程。长进程返回 session id 时必须保留句柄，直到获得最终结果。
- 不提交凭据、密钥、日志、缓存、个人目录和机器身份。文档使用仓库相对路径、`%USERPROFILE%` 或公开环境变量。
- 修改结构化数据时使用 JSON/Schema/领域 parser，禁止字符串替换。

完整规则：`AgentHarness/policies/safety.md`。

## 架构底线

AvidScript 是面向 UE 的 WASM-first、C#-first、PC-first 且 mobile-aware 的现代游戏脚本框架。目标是形成可用于真实游戏逻辑的完整闭环，并在关键路径性能、可维护性和安全性上领先现有方案。

不可违反：

- C# 前端使用 Roslyn 获取语法与语义，输出版本化 Guest IR；Roslyn 不进入 UE Runtime，Runtime 不托管 CLR。
- Guest IR、WASM ABI、binding descriptor 和 UE Runtime 是独立版本化合同，依赖方向保持单向。
- VM backend 通过稳定接口隔离。Runtime/Bindings 不泄漏 WAMR、Wasmtime 或其他 backend 具体类型。
- 一般 UE API 通过 Reflection、递归 type capability、descriptor、codec program 与 generated facade 覆盖，不逐个手写 `UFunction`/类型包装。
- `ObjectHandle` 不是裸 `UObject*`。跨边界访问必须验证 registry、代际、类型、world、线程和生命周期。
- Guest memory、descriptor、Schema、import/export、动态调用和复合容器全部 fail-closed；被验证的 bytes 必须就是被执行的 bytes。
- Session 管理实例、callback、continuation、BeginPlay、Tick、event、reload、cancel 与 teardown。所有清理幂等，失效 Session 不得再次进入。
- UObject 交互默认在 Game Thread。Runtime 不依赖 editor-only API，并保留 packaged/mobile 边界。
- 热路径把反射发现、route 选择和 codec 编译放入冷路径，避免每调用名称查找、分配、重复 `FProperty` 遍历和多余 crossing。

领域细则由 Harness 根据任务路由：

| 领域 | 策略包 |
| --- | --- |
| C#、Roslyn、Semantic、Lowering | `AgentHarness/domains/csharp-frontend.md` |
| Guest IR、ABI、WASM emitter、Guest Memory | `AgentHarness/domains/guest-ir.md` |
| Reflection、UFunction、UProperty、Descriptor | `AgentHarness/domains/bindings.md` |
| BeginPlay、Tick、Event、Reload、ObjectHandle | `AgentHarness/domains/runtime.md` |
| WAMR、Wasmtime、JIT/AOT、Host Import | `AgentHarness/domains/vm-backends.md` |
| Benchmark、Puerts/AngelScript 对比、性能门禁 | `AgentHarness/domains/performance.md` |

## Phase 生命周期

`Build/InvokePhaseWorkflow.ps1` 和 `Docs/Phase*/Phase*_State.json` 是唯一 Phase 状态机；Harness 只做发现、路由与调用，不维护第二份状态。

固定顺序：

1. 冻结目标、非目标、模块边界、公开 API、Schema、ABI、生命周期、平台和 Gate。
2. 按完整能力与独立写集组织 2 至 4 个实现批次；产品代码、必要测试和最小文档一起落盘。
3. 集成后冻结代码，执行一次集中代码与架构审查，一次列全 findings。
4. 合并为一次集中修复；只有新 Blocker/Critical 才追加最小探针。
5. 阶段末统一执行静态检查、受影响 .NET、no-clean UBT、Automation 和必要 benchmark。
6. 每个架构、功能、集中修复或 Gate 小节完成集中验证后，同步 README 的已实现/未实现边界，立即白名单提交并推送；阶段末再更新中文收尾、Gate 证据和 Phase 状态。

禁止把阶段拆成“小任务实现 -> 独立复审 -> 再复审”的串行链。普通问题进入阶段债务；只有崩溃、数据破坏、安全、ABI/Schema、生命周期、并发、跨平台或工具链阻塞可以打断实现。

架构和写集冻结后才并行；并行写任务使用独立 worktree/branch，或证明写集完全不重叠。完整定义见 `AgentHarness/policies/phase-workflow.md` 和 `Docs/Workflow/Phase_Development_Workflow_Design.md`。

## 验证与证据

- 实现期保留廉价编译反馈、静态检查和高风险最小探针；完整 UE 构建、Automation 和 benchmark 默认在阶段 Gate 集中执行。
- UE 构建默认 no-clean、`-WaitMutex`、`-NoHotReloadFromIDE`。不得清 Editor target。隔离 target 后先恢复 canonical target，再读取 BuildId。
- AvidScript 的自执行 .NET 测试项目使用固定 SDK 的 `dotnet run`，证据必须包含 runner 自报 passed/total；静默 `dotnet test` 不算执行。
- Automation 使用受控模板和唯一 `-abslog`，同时核对完整测试数、失败数、队列完成和进程退出。
- 构建、静态检查和 Automation 不是 PIE、控制器、网络、视觉或真实游戏流程的替代品；人工 Gate 要明确保留。
- 性能比较冻结 workload、配置、runtime mode、warmup、样本和统计口径；分别报告纯执行、crossing、reflection、property、event/Tick 和分配成本。
- 候选 tree 改变、日志不完整、进程被中断或缺少结束标记时，旧证据立即失效。

完整规则：`AgentHarness/policies/verification.md`。

## 文档与交付

- 面向用户和团队阅读的计划、架构、实现、验收与使用文档默认使用中文；代码标识符、路径、命令和日志原文保持原语言。
- 每个交付组只记录范围、关键文件、验证命令、结果、剩余风险和下一步。不要把流水日志复制进主文档。
- README 的功能、成熟度和性能声明必须能追溯到当前代码与可复现实验，不把计划写成已完成；每个功能小节验证通过后用简短条目同批同步，不等待整个 Phase。
- 提交前使用精确路径白名单检查 staged Diff；不混入用户私有改动、进行中其他 Phase 或不必要生成文件。

## Harness 与历史

- 路由清单：`AgentHarness/manifest.json`。
- 可执行入口：`Build/InvokeAgentHarness.ps1`。
- 通用教训索引：`AgentHarness/lessons/rules.json`，查询结果最多 5 条。
- 旧 `AGENTS.md` 原文：`AgentHarness/lessons/legacy-agents-2026-08-30.md`。仅在索引无法回答时定向检索，不整文件载入上下文。
- 原文归档必须与 manifest 中的字节数和 SHA-256 一致。`audit` 负责入口大小、链接、重复 ID、未注册策略和归档完整性。

新增规则时先判断：可机械验证的进入 Harness；稳定架构语义进入对应策略包；事故记录进入 `rules.json` 或 legacy 历史。根 `AGENTS.md` 只在核心边界变化时修改，并保持不超过 12 KiB，硬上限 16 KiB。
