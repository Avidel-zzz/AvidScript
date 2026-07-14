# Phase 40 语义分析与控制流收尾

状态：完成
日期：2026-07-14

## 阶段目标

Phase 40 把 Phase 39 的 Roslyn syntax frontend 提升为可供后端消费的稳定语义边界。C# 构建链现在不再只判断“语法能否解析”，而是必须通过类型、符号、typed operation、conversion、ControlFlowGraph、实例 identity 与支持策略检查，才允许进入后续 emitter。

本阶段没有扩张逐个手写的 UE API，也没有把类型推断或控制流逻辑搬进 PowerShell。Roslyn 仍是 C# 语义真相，AvidScript JSON 只是版本化、确定性的投影。

## 已完成的闭环

正式构建顺序为：

1. `InvokeCSharpFrontend.ps1` 生成 `<stem>.csharp.frontend.json`。
2. `InvokeCSharpSemantic.ps1` 调用 .NET 8 semantic CLI，生成 `<stem>.csharp.semantic.json`。
3. semantic artifact 必须 `succeeded=true`，source hash 必须与 frontend 一致。
4. 只有通过 semantic gate 后，构建链才进入当前过渡态 AST emitter 或后备 .NET/WASI 路径。
5. manifest 与 report 同时记录 frontend/semantic artifact、schema、version 和 source hash。

语法错误在 frontend gate 失败；类型错误、不支持语义或 CFG 不完整在 semantic gate 失败。两类失败都返回 1，并且不会留下可被 Editor 或 Runtime 误加载的旧 manifest/WASM。

## Semantic Artifact

当前契约为 schema v3 / semantic 1.3，包含：

- canonical type table；
- 完整 containing identity 的 symbol table；
- Roslyn 绑定的 typed operation 与 conversion；
- 稳定基本块、successor edge 与 flow capture ID；
- constructor、instance field、显式/隐式 `this` identity；
- compiler、ASCS2xxx、ASCS3xxx、ASCS4xxx 诊断；
- source SHA-256 与 frontend SHA-256 双重来源校验。

真实 ActorLifecycle 当前为 15 types、186 symbols、38 methods、38 CFG、120 blocks、84 edges、0 diagnostics。构建集成中 semantic、frontend 与 report 的 source hash 均为 `5a14f6c5a42a9e704e45090f45fc4f269d356aae760b01ee253a63388823b5a7`。

## 自定义 Profile 与 Reference Source

UE BuildService 的自定义 `CustomMoverScript.cs` 首次接入 semantic gate 时暴露了 `CS0103`：自定义文件引用 `AvidScript.Actor`，但 semantic compilation 当时只包含主文件。

现在 semantic CLI 支持可重复的 `--reference-source`。reference source 只参与 Roslyn Compilation，AvidScript 仍只投影主脚本的声明、operation、CFG 与主 source span；内部 reference source id 不包含绝对路径。当前构建链在分析非默认 profile 时临时复用现有 ActorLifecycle facade 源作为 reference。

这只是 Phase 40 到 Phase 42 的兼容桥，不是最终 SDK 设计。Phase 42 必须由 UE Reflection Binding Generator 生成独立 C# reference API/proxy/host stub，并删除对 sample facade 的临时依赖；禁止继续手工扩张该 facade。

## Report 与 Editor

C# report 新增：

- `artifacts.semantic_file`；
- `semantic.schema_version`；
- `semantic.version`；
- `semantic.succeeded`；
- `semantic.source_sha256`；
- `semantic.frontend_sha256`；
- `semantic.diagnostic_count`。

`FAvidScriptFrontendReportReader` 已读取这些字段。缺少 `semantic` 对象的 Phase 39/legacy report 仍可加载，字段保持空字符串、0 与 false，不改变旧流程。

## TDD 与故障收敛

RED 首先证明两项旧行为确实存在：

- 语义错误脚本仍返回 0；
- Editor report 结构没有 semantic metadata。

GREEN 接入后，BuildService 的真实自定义 profile 又发现 reference context 缺失。新增 reference-source CLI 测试先以 `Unknown option: --reference-source` 失败，随后完成实现并使 BuildService 恢复 2/2。

永久构建集成覆盖：

- syntax error 保留 frontend 诊断且不生成 manifest/WASM；
- semantic error 返回 1、保留 semantic artifact 与 `CS0029`，并删除旧 manifest、adapter WASM、dotnet WASM；
- 正常构建生成 frontend/semantic/report/manifest/WASM，三方 hash 一致；
- manifest 显式引用 semantic artifact。

## 验证结果

| 验证项 | 结果 |
| --- | --- |
| .NET SDK | 精确 8.0.416，`global.json` 为 `rollForward=disable` |
| Semantic 行为与 CLI | 34/34 成功 |
| Frontend 回归 | 7/7 成功 |
| BuildIntegration | 3/3 成功 |
| dotnet format | 两个测试工程 `verify-no-changes` 成功 |
| UE5.8 Runtime 模块 | 非 Clean 增量构建成功 |
| UE5.8 Editor 模块 | 非 Clean 增量构建成功 |
| Editor Report | 5/5 成功 |
| C# Guest | 4/4 成功 |
| C# BuildService | 2/2 成功 |
| 完整 AvidScript automation | 139/139 成功，`Result={Fail}` 为 0 |

本阶段没有执行 package/cook，遵循当前“打包先跳过”的决定。

## Phase 41 输入

Phase 41 可以把 semantic schema v3 / 1.3 作为唯一 C# 语义输入，但必须继续执行以下硬门：

- semantic `succeeded=true`；
- source/frontend hash 一致；
- 目标 method 存在匹配 CFG；
- 不直接读取 Roslyn 对象、数字枚举或临时 SDK 路径；
- 不从 C# 文本重新猜测 symbol、conversion 或控制流。

Phase 41 的任务是设计稳定 Guest IR、实例/静态状态布局和确定性 codegen，并逐步替换当前过渡 AST emitter。Phase 42 再接 UE Reflection Binding Generator，形成可规模化的 UE API 面，而不是逐个手写 binding。
