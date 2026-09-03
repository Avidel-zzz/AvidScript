# 安全策略

## 用户改动

- 当前工作树可能同时包含用户改动、进行中的 Phase 和本任务改动。默认全部保留。
- 当前 Phase state 的 `protected_dirty` 是基线合同。Harness 负责比较状态和内容哈希；发生漂移时停止相关写入并明确报告。
- 用户已授权且已经提交的路径可在实现期通过 `adopt-protected-path` 精确移交；必须提供提交证据和原因，不能重采整个基线。详见 `Docs/Workflow/Protected_Path_Adoption.md`。
- 不使用 `git reset --hard`、`git checkout --`、`git clean` 或等价破坏性操作，除非用户明确授权具体目标。
- 不结束、清理或重启用户的 Unreal Editor、编译器、dotnet 或其他进程。只有已证明归属本任务的子进程才能由其 owner 管理。

## 构建与资产

- 默认不清理 Editor target、Binaries、Intermediate、DerivedDataCache 或引擎输出。
- 不以“解决 BuildId”作为清理理由；先确认目标顺序、模块 identity 和 canonical target 恢复流程。
- 不直接修改 `.uasset`、`.umap` 或生成二进制；需要资产操作时使用 UE 支持的工具并保留人工视觉验收。
- Runtime 不依赖 editor-only API；PC-first 实现仍要保留 packaged/mobile 的边界。

## Git 与发布

- 不提交密码、token、私钥、用户目录、日志、缓存或个人身份信息。
- 文档和脚本使用仓库相对路径、`%USERPROFILE%` 或显式环境变量，不写用户名目录。
- 不改写提交历史、不 force-push、不删除分支或 tag，除非用户明确要求并确认范围。
- 推送前检查远端、分支、提交范围和 staged Diff；README 中的性能声明必须能追到可复现实验。

## 运行时安全

- WASM Guest 输入、descriptor、handle、memory range、Schema 和动态调用均 fail-closed。
- `ObjectHandle` 不是裸 `UObject*`；必须经过 registry、代际或等价有效性验证。
- 所有权、取消、reload、continuation、BeginPlay/Tick/event 注入和线程切换必须有明确生命周期 owner。
- 对失败路径使用稳定错误码和可诊断上下文，不把异常输入静默降级为成功。
