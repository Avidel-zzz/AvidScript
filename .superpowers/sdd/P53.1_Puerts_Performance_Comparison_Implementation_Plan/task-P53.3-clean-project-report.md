# P53.3 Clean Benchmark Project 报告

## 状态

实现完成。新增 bootstrap 只会在临时输出根下创建全新的 attempt 目录，复制单个 `.uproject`，建立五个受限 junction，校验候选 AvidScript worktree 的 commit/tree/cleanliness，并以 UTF-8 no-BOM 原子发布 `benchmark-project.json`。

按 brief 约束，未运行 UBT、Unreal Editor 或任何重型验证；结论仅基于 PowerShell parser、临时 fixture contract 和 `git diff --check`。

## 修改文件

- `Benchmarks/PuertsComparison/Scripts/New-PuertsBenchmarkProject.ps1`
- `Benchmarks/PuertsComparison/Scripts/Test-PuertsBenchmarkProject.ps1`
- `.superpowers/sdd/P53.1_Puerts_Performance_Comparison_Implementation_Plan/task-P53.3-clean-project-report.md`

未修改 sidecar 既有脚本、Schema、Harness C++、项目文件或 production module。工作树中另有既存修改 `Benchmarks/PuertsComparison/Scripts/Test-PuertsBenchmarkSidecarContracts.ps1`，本任务未触碰。

## 设计决策

- `New-PuertsBenchmarkProject.ps1` 只接受 canonical absolute inputs；对 source `.uproject`、候选 AvidScript、Pinned Puerts、tracked Harness 分别做类型与标志文件校验。
- 候选 AvidScript 必须是 linked Git worktree 根目录，且 `HEAD`、`HEAD^{tree}`、`git status --porcelain=v1 --untracked-files=all` 必须与输入固定值完全一致，否则以 `ASP53B11xx` 拒绝。
- attempt 目录采用 UTC 时间戳加递增序号分配，绝不复用现有目录；任何 source plugin 与 destination 的包含关系都会以 `ASP53B1200` 拒绝。
- bootstrap 只复制 `.uproject` 文件；`Source`、`Config`、`Plugins/AvidScript`、`Plugins/Puerts`、`Plugins/AvidScriptPerfHarness` 全部使用 junction，且不暴露自动递归删除命令。
- marker 使用同目录临时文件加原子 rename 发布，内容固定 `schema_version`、`created_utc`、项目文件名、candidate commit/tree 和五个 junction 目标。
- focused contract 使用临时 fixture 目录和临时 Git repo/worktree，覆盖 happy path、五个 junction、marker 身份、dirty/commit/tree rejection、attempt 不复用、overlap rejection、生成目录不泄漏，以及脚本不包含个人绝对路径。

## 验证命令与输出

1. RED：`Test-PuertsBenchmarkProject.ps1`
   - 预期失败：`ASP53BT1000 missing source file ... New-PuertsBenchmarkProject.ps1`
2. PowerShell parser：
   - `Puerts benchmark clean project parser passed: files=2`
3. Focused fixture contract：
   - `Puerts benchmark clean project contracts passed: parser=2 happy_path=1 junctions=5 marker=1 dirty_rejection=1 commit_rejection=1 tree_rejection=1 attempt_reuse_rejection=1 overlap_rejection=1 generated_dirs=4 privacy=1`
4. `git diff --check`
   - exit code 0；仅提示仓库既有 `Test-PuertsBenchmarkSidecarContracts.ps1` 的 LF/CRLF checkout warning，本任务未修改该文件

## 关注事项

- bootstrap 当前要求候选 AvidScript 根目录存在 `AvidScript.uplugin`，Pinned Puerts 根目录存在 `Puerts.uplugin`，Harness 根目录存在 `AvidScriptPerfHarness.uplugin`；如果后续仓库布局变化，需要同步调整合同与 fixture。
- brief 明确禁止自动清理；当前实现只负责创建与标记 attempt。后续如需 cleanup，仍需单独设计带 junction-aware path 校验的命令。
