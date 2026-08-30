# AvidScript 阶段开发流程设计

> 状态：已批准
>
> 批准日期：2026-07-22
>
> 适用范围：AvidScript 插件后续所有 Phase

## 1. 背景

AvidScript 已进入需要持续扩展语言前端、WASM 后端、反射绑定、运行时生命周期和 UE API 覆盖的阶段。此前开发过程在每个微任务后频繁启动 UBT、Unreal Editor Automation 和多组脚本测试，虽然产生了大量局部证据，但也造成以下问题：

- 实现工作被频繁的构建和测试等待切碎。
- 相同模块和测试组在代码尚未冻结时被重复验证。
- 审查发现修改后，之前的构建和测试证据随即失效。
- 每个微任务分别维护计划、结果和验证文档，文档成本高于其决策价值。
- 网络中断或任务恢复后，缺少统一检查点，容易重新执行已经通过的耗时门禁。

本设计采用“阶段冻结后统一验收”的默认模式。它不会降低阶段最终质量门槛，而是把验证安排到信息量最高、代码最稳定的时点。

## 2. 目标

- 将架构分析和产品代码占有效工作时间的比例提高到 60% 以上。
- 将构建和测试等待控制在有效工作时间的 25% 以内。
- 普通问题不打断阶段实现主线，高风险问题仍能立即阻断。
- 每项昂贵验证在输入未变化时只执行一次。
- 阶段结果可恢复、可审计，并由机器证据决定是否完成。
- 面向团队的计划、实现和验收文档保持中文且数量可控。

## 3. 非目标

- 不降低完整 UE Automation、模块构建、工具链测试和架构检查的最终覆盖。
- 不允许为了保持阶段速度而忽略崩溃、数据破坏、安全或 ABI 风险。
- 不要求现阶段立即部署远程 UE 构建集群或 GitHub 自托管 Runner。
- 不把所有 Phase 强制压成相同工期；风险和模块跨度仍决定工作量。

## 4. 核心原则

1. 先冻结架构契约，再连续实现。
2. 以少量完整功能组划分批次，不以单个测试、文件或小型 finding 划分任务。
3. 接口冻结后，独立写集使用隔离 worktree/branch 并行实现。
4. 阶段中途只处理阻塞性高风险问题。
5. 普通缺陷进入阶段债务清单，在代码冻结后集中修复。
6. 整个阶段只做一次集中审查和一个集中修复批次，再执行一次昂贵验收。
7. 只有最终 Gate 可以将 Phase 标记为完成。

## 5. 阶段生命周期

### 5.1 架构冻结

进入编码前必须明确：

- 阶段目标与非目标。
- 涉及的模块、所有权边界和依赖方向。
- 对外 API、生成物 Schema、ABI、生命周期和线程契约。
- 是否涉及 Guest Memory、序列化、状态迁移或持久化。
- 最终验收需要执行的测试组、构建目标和架构门禁。
- 阶段起始 Git 提交，用于后续变更影响分析。

架构存在关键歧义时不得开始大规模实现。实现过程中需要改变已冻结契约时，应先更新设计和风险分类。

### 5.2 实现批次

每个 Phase 默认拆分为 2 至 4 个可并行功能组，每组以可交付能力和独立写集为边界，不以单个文件、测试或微小修复继续细分。功能组应同时包含完成该能力所需的产品代码、必要测试代码和最小文档记录。

架构、模块边界、公开接口、Schema、ABI 和验收指标冻结后，应立即为独立写集建立隔离 Git worktree/branch 并行开发。确需共享工作树时，必须事先证明写集不重叠，并在提交前核对精确 staged path；无法证明时不得并行写。

接口已冻结的编码代理不得长时间停留在开放式分析。代理开始后 10 分钟内必须产生第一份代码 Diff，或者返回精确 blocker、涉及路径和已核对证据；超时仍无文件变化时，控制器应立即中断、缩小任务或收回主线。深度推理代理保留给架构冻结、跨层高风险判断和阶段末集中审查，边界明确的实现交给快速实现代理。

批次内默认允许：

- 编译器或静态类型检查。
- 格式和语法检查。
- 不启动 UE 的快速工具测试。
- 针对高风险契约的廉价探针。
- 阅读代码、Diff 和生成物。

批次内默认不执行：

- 每个功能组完成后的独立审查或再审查。
- 完整 UE Automation。
- 完整 Editor Target 构建。
- 对未受影响模块的重复构建。
- 仅为了获得临时绿色状态而进行的全量回归。
- 为穷举低风险 flag 组合而编写或运行的无意义测试。

### 5.3 风险分流

发现问题后先分类，而不是立即中断当前批次。

| 级别 | 判定标准 | 阶段中途处理 |
| --- | --- | --- |
| Blocker | 工具链或核心路径无法继续运行，后续实现没有可信基础 | 立即修复 |
| Critical | 可能引发崩溃、数据破坏、安全问题、ABI 破坏或架构契约失效 | 立即修复并执行最小风险探针 |
| Important | 行为错误、测试失败、明显性能退化，但不会使后续实现失去基础 | 进入债务清单，代码冻结后集中修复 |
| Normal | 文档、诊断质量、样式、低风险边界或非关键优化 | 进入债务清单，阶段关闭前处理或明确转移 |

下列领域默认视为高风险候选，需要显式判断是否立即探测：

- Host 与 Guest ABI。
- 对外 Schema 和向后兼容性。
- Guest Memory 边界和对象句柄有效性。
- 状态迁移、序列化和事务回滚。
- BeginPlay、Tick、事件分发和 Runtime 生命周期。
- 跨线程所有权、取消和进程树管理。
- Runtime 对 editor-only API 的依赖，以及 packaged/mobile target 编译。
- 可能导致 Editor 崩溃或破坏项目资产的操作。

### 5.4 代码冻结

计划功能完成后进入代码冻结：

1. 停止增加本 Phase 能力。
2. 审核完整差异和模块依赖。
3. 对整个集成候选执行唯一一次集中代码与架构审查，并一次列全 findings。
4. 汇总并清算阶段债务。
5. 将审查问题和债务合并为一个集中修复批次。
6. 修复完成后锁定用于最终 Gate 的提交。

不得为普通 Task 或单个 finding 启动“审查 -> 修复 -> 再审查”循环。代码冻结后的普通修复不应逐项触发复审或完整构建；只有修复本身暴露新的 Blocker 或 Critical 风险时才执行额外最小探针。

### 5.5 统一验收

最终 Gate 按以下顺序运行：

1. 静态架构检查和格式检查。
2. 所有受影响的 .NET 测试宿主。
3. 受影响 UE 模块的 no-clean 构建。
4. 四模块集成构建，适用于跨 Core、VM、Runtime、Editor 的变化。
5. 一个 Unreal Editor 进程内运行完整 `Automation RunTests AvidScript`。
6. 校验测试数量、失败数量、队列结束状态和进程退出状态。
7. 生成机器可读结果和中文验收报告。

完整 Gate 正常只执行一轮。若失败：

- 只修复失败组及其直接依赖。
- 先运行受影响的失败检查。
- 修复影响公共行为或共享契约时，再执行一次完整 Gate。
- 常规 Phase 最多执行两轮完整 Gate；超过时必须记录流程或架构原因。

### 5.6 阶段关闭

Phase 完成必须同时满足：

- 没有 Blocker、Critical 或 Important 债务。
- 所有最终 Gate 项通过。
- 生成物、文档和实现对应同一个已验证提交。
- 阶段计划已标记完成。
- 中文收尾文档记录能力、限制、验证证据和后续风险。
- Git 提交范围不包含用户私有改动、凭据、本机账户信息或非必要生成文件。

Normal 债务只有在不影响阶段交付目标时才能转移，并必须记录目标 Phase 和处理理由。

## 6. 阶段债务清单

每项债务至少包含：

| 字段 | 含义 |
| --- | --- |
| ID | 阶段内唯一编号 |
| 严重级别 | Blocker、Critical、Important 或 Normal |
| 发现批次 | 首次发现问题的实现批次 |
| 影响范围 | 模块、API、平台或工作流 |
| 证据 | 日志、失败测试、代码位置或复现步骤 |
| 延期原因 | 为什么不在发现时修复 |
| 修复建议 | 预计修复方向和影响测试 |
| 状态 | Open、Fixing、Verified 或 Transferred |

债务清单是控制上下文切换的工具，不是隐藏失败的地方。进入代码冻结后，Important 以上问题必须进入集中修复。

## 7. 统一 Gate Runner

计划新增统一入口：

```powershell
./Build/RunPhaseGate.ps1 -Phase 49 -Mode Final
```

### 7.1 执行模式

- `Probe`：只运行阻塞性高风险探针。必须提供风险原因和目标契约。
- `Final`：对冻结提交执行完整阶段验收。
- `Resume`：从上一次检查点继续，仅重跑失败或已失效项目。

### 7.2 变更影响分析

Runner 根据阶段起始提交和当前 `HEAD` 之间的文件差异构建影响集合：

| 变化范围 | 默认验证 |
| --- | --- |
| C# 编译器、生成器或工作区工具 | 对应 .NET 测试宿主、格式和 Schema 检查 |
| Core、VM 或 Binding | 对应模块构建、ABI 或 Binding 契约检查 |
| Runtime | Runtime 构建、生命周期和运行时自动化 |
| Editor 或 Developer | 对应模块构建、编辑器集成自动化 |
| ABI、Schema、Cache、Guest Memory | 额外执行版本和兼容性探针 |
| 跨四个插件模块 | 四模块 no-clean 构建和完整 AvidScript Automation |

影响规则必须保守：无法确认影响范围时扩大验证，不允许静默跳过。

### 7.3 缓存与失效

每个 Gate 项的缓存键至少包含：

- 当前 Git tree 哈希。
- 验证命令和参数。
- Runner 版本。
- 直接输入文件指纹。
- 使用的 .NET SDK、UE 引擎和目标配置。

输入未变化且结果完整时允许复用。以下情况必须使结果失效：

- 相关源码、测试、配置或生成器发生变化。
- 验证命令或 Runner 版本变化。
- SDK、UE BuildId、Target 或平台变化。
- 上一次运行被中断，且没有完整结束标记。

### 7.4 日志和恢复

日志默认写入：

```text
C:/tmp/AvidScript/<Phase>/<RunId>/
```

每次运行保存：

- 阶段 ID、起始提交和验证提交。
- 命令、参数、开始和结束时间、退出码。
- 测试总数、成功数、失败数和未执行数。
- 构建模块和目标。
- 缓存命中及失效原因。
- 债务清单快照。
- 下一项可恢复检查点。
- 最终 JSON 与中文 Markdown 摘要。

网络断开或任务恢复后，先读取检查点和当前 Git tree。只有二者匹配时才允许 Resume。

## 8. 等待时间利用

UBT 或 UE Automation 运行期间，只进行不会使当前证据失效的工作：

- 编写或整理中文阶段文档。
- 审计 Diff、依赖和公开 API。
- 整理性能数据和覆盖率统计。
- 分析下一阶段架构。
- 检查提交范围、隐私和不必要文件。

验证期间修改相关源码会使对应结果失效。Runner 应检测 tree 变化，并在报告中拒绝把旧结果用于阶段关闭。

## 9. 提交策略

每个 Phase 按可独立审查的小节形成逻辑提交，不设置为了压缩数量而延迟提交的硬上限。通常包括：

1. 架构和契约。
2. 两至四个功能实现批次。
3. 集中修复和验证。
4. 阶段文档和关闭记录。

架构、功能、集中修复或 Gate 小节完成其集中验证后，应立即使用精确路径白名单提交并推送 `origin/main`，不等待整个 Phase 结束。提交前必须确认 protected dirty 没有进入 staged Diff，并检查凭据、个人路径和不必要生成物。

不为单个测试、一次构建修复或微小文档调整建立碎片提交；它们归入所属小节。实现提交仍需保持可审查，不允许把多个无关模块压成一个无法理解的提交。push 失败时保留本地提交并优先恢复远端同步，禁止通过强推或改写历史解决分歧。

## 10. 文档策略

每个 Phase 默认维护三份中文核心文档：

1. 架构设计与关键决策。
2. 实施计划与批次状态。
3. 阶段验收与遗留风险。

只有长期架构决策发生变化时才新增 ADR。测试日志由 Gate Runner 生成，不再人工复制到多个微任务结果文档。

## 11. 量化指标

常规 Phase 的流程目标：

| 指标 | 目标 |
| --- | --- |
| UE Automation 启动 | 通常 1 次，最多 2 次 |
| UBT 构建 | 通常 2 次，最多 3 次 |
| 完整 Gate | 通常 1 轮，集中修复影响公共行为时最多 2 轮 |
| 实现批次 | 3 至 5 个 |
| 架构分析和产品编码 | 不少于有效工作时间的 60% |
| 构建和测试等待 | 不超过有效工作时间的 25% |
| 阶段结束债务 | Blocker、Critical、Important 均为 0 |

指标用于暴露流程问题，不用于削减必要覆盖。UE 源码构建等客观原因导致超标时，应在阶段收尾中记录原因。

## 12. Phase 48 参考基线

Phase 48 曾产生约 60 次 UE Automation 进程和 34 份 UBT 构建日志。采用本流程后，同等规模阶段的预期目标为：

- UE Automation 启动约 1 至 3 次。
- UBT 调用约 2 至 4 次。
- 构建和测试等待由约 1 小时 46 分钟降低到约 30 至 50 分钟。
- 节省的有效时间投入语言能力、绑定架构、UE API 覆盖和性能路径。

这些数字是流程目标，不是未经测量的保证。应使用后续两个完整 Phase 的实际数据校准。

## 13. 风险升级策略

方案 A 是默认流程。出现以下任一条件时，可将当前 Phase 临时升级为“中点集成门禁”：

- 有效编码预计超过 4 小时且跨越多个底层契约。
- 同时修改语言前端、Guest IR、VM ABI 和 Runtime。
- 需要迁移已有脚本状态或生成物 Schema。
- 高风险探针无法对关键假设提供足够证据。

升级只增加一次中点定向门禁，不恢复微任务级完整验证。

## 14. 后续实施边界

本文件定义流程和 Runner 合同，不包含 Runner 实现。设计经审阅后，后续实施计划应至少覆盖：

- 仓库级阶段状态 Schema 和唯一下一步。
- `InvokePhaseWorkflow.ps1` 状态转换入口。
- `RunPhaseGate.ps1` 的模式、参数和退出码。
- 变更影响图和验证注册表。
- 检查点、缓存、失效与 Resume。
- JSON 和中文 Markdown 报告 Schema。
- 阶段债务清单模板。
- AGENTS.md 与现有 Build 脚本的接入。
- 使用一个完整 Phase 对指标进行试运行和校准。

在 Runner 实现完成前，团队应手工遵循本设计的生命周期和风险分流规则。

## 15. 执行约束与跨会话恢复

流程不能只依赖当前对话、操作者记忆或人工自觉。AvidScript 使用“仓库指令、机器状态、命令状态机、关闭门禁”四层约束，保证后续任务和中断恢复仍能遵循相同标准。

### 15.1 仓库指令

`AGENTS.md` 必须引用本设计，并要求每次开始新 Phase、恢复任务或发生上下文压缩后先读取：

1. 当前 Phase 状态。
2. 当前实现批次。
3. 未关闭债务。
4. 最近一次 Gate 证据。
5. 唯一下一步。

阶段状态以仓库文件为准，不以聊天总结作为完成依据。

### 15.2 阶段状态文件

每个 Phase 保存机器可读状态：

```text
Docs/Phase<Number>/Phase<Number>_State.json
```

状态至少包含：

- Phase ID、目标和起始提交。
- 当前生命周期状态。
- 架构冻结版本。
- 实现批次及完成状态。
- 债务条目和严重级别。
- 已执行的 Probe、UBT 和 UE Automation 次数。
- 代码冻结提交。
- Gate Run ID 和验证提交。
- 报告路径。
- 唯一 `next_action`。

状态文件由命令更新，人工文档从状态生成或引用状态，避免出现多个互相矛盾的进度来源。

### 15.3 命令状态机

统一入口已经实现为：

```powershell
./Build/InvokePhaseWorkflow.ps1 start -Phase <Number> ...
./Build/InvokePhaseWorkflow.ps1 status -Phase <Number>
./Build/InvokePhaseWorkflow.ps1 batch-complete -Phase <Number> ...
./Build/InvokePhaseWorkflow.ps1 debt-add -Phase <Number> ...
./Build/InvokePhaseWorkflow.ps1 debt-update -Phase <Number> ...
./Build/InvokePhaseWorkflow.ps1 architecture-revise -Phase <Number> ...
./Build/InvokePhaseWorkflow.ps1 freeze -Phase <Number> ...
./Build/InvokePhaseWorkflow.ps1 attest -Phase <Number> -GateReportPath <Path>
./Build/InvokePhaseWorkflow.ps1 close -Phase <Number>
./Build/InvokePhaseWorkflow.ps1 reopen -Phase <Number> ...
```

状态转换必须满足前置条件。例如：

- 未冻结架构时不能进入实现批次。
- 存在 Blocker 或 Critical 时不能完成当前批次。
- 计划批次未完成时不能冻结代码。
- Gate 只能验证包含 `gate_ready` 状态的冻结候选提交。
- `attest` 只接受验证当前 `HEAD`、tree 和 state hash 的不可变 Gate 报告。
- Close 只接受父提交恰好为 Gate verified commit 的单个 attestation commit。

`status` 输出应足以让新的任务或重连后的执行者直接继续，不需要重建完整对话上下文。

### 15.4 Gate 与 attestation 两提交模型

Git commit 不能在自身内容中保存自己的 commit hash，因此阶段关闭采用可验证的两提交模型：

1. 所有产品代码、测试和性能实现提交完成后执行 `freeze`，记录冻结前源码提交、tree 和输入 state hash。
2. 提交 `gate_ready` 状态，形成 Gate candidate commit。
3. 完整 Gate 只验证该 candidate commit，并把 commit、tree、state SHA-256、日志 hash、计数和退出状态写入仓库外不可变 JSON。
4. 在 candidate commit 上执行 `attest`，状态记录 Gate identity 和 `attestation_parent`。
5. 只允许一个 attestation commit 修改当前 Phase state、closeout 和归一化 Gate summary。
6. Close 从当前 `HEAD` 推导真实 attestation commit，并证明其唯一父提交等于 Gate verified commit。

`freeze.source_commit` 是执行 freeze 前已经提交的产品源码身份；Gate verified commit 是随后包含 `gate_ready` 状态的候选提交。`attestation_parent` 保存可提前知道的 verified commit，真实 attestation commit 只写入仓库外 Close evidence，避免任何提交自引用。

### 15.5 关闭门禁

Close 必须拒绝以下情况：

- 当前提交不是 Gate verified commit 的直接 attestation 子提交。
- 相关源码在验证后发生变化。
- 仍存在 Blocker、Critical 或 Important 债务。
- 测试数量、Automation 队列、退出码或完成标记不完整。
- 验收报告或必要生成物缺失。
- 构建和测试调用超过阶段预算且没有风险升级说明。
- 提交范围包含凭据、本机账户信息、私有路径或非必要生成文件。

Close 成功时只写仓库外不可变关闭证据，不再修改已验证或已确认的 tracked tree。只有 Close 成功后，Phase 才能在计划和对外报告中标记为完成。

### 15.6 恢复协议

每次任务恢复固定执行：

```powershell
./Build/InvokePhaseWorkflow.ps1 status -Phase <Number>
```

Runner 比对状态文件、Git tree、检查点和最近报告：

- 全部匹配时，从 `next_action` 继续。
- 源码变化时，使受影响的 Gate 证据失效。
- 运行被中断时，只恢复具有完整前置证据的下一项。
- 状态互相矛盾时停止 Close，但允许执行只读诊断和状态修复。

Phase 49 使用：

```powershell
./Build/InvokePhaseWorkflow.ps1 status -Phase 49
```

只有目标 Phase 的状态文件尚不存在时，才允许暂时读取当前计划和债务清单手工恢复；状态创建后不得以聊天记录覆盖机器状态。

### 15.7 可执行性边界

仓库指令约束代理行为，状态机保存事实，Gate Runner 产生证据，Close 阻止错误完成。任何单层都不能独立保证流程：

- 文档无法阻止遗漏。
- 状态文件不能证明测试真实完成。
- 测试通过不能证明验证的是当前提交。
- 对话记忆不能承担跨任务恢复。

因此后续实施必须先完成状态机和关闭门禁，再优化缓存、并行和远程 CI。
