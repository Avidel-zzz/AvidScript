# Phase 61 收尾记录

状态：实施中，`P61.A1-A2` 已完成

## 目标

交付增量编译、结构化错误定位、指令级 source map、调用栈、断点、变量查看、Profiler 与 IDE workspace，并保持 WASM-first、后端无关和 Session-owned 生命周期。

## 已完成

- 完成现有能力盘点：semantic cache、function-level DebugMap、Runtime mapped stack、Project workspace、异步构建与 live reload 可直接复用；
- 冻结 P61.A-E 分层边界，禁止第二套编译器、scheduler 和只靠日志伪装 debugger。
- 完成 `P61.A1` 完整编译产物缓存：无修改热构建的四个编译阶段均为零调用，损坏条目可隔离并回退重建；
- 详细实现与证据见 [P61.A1 完整编译产物缓存](P61.A1_Complete_Compilation_Cache.md)。
- 完成 `P61.A2a` persistent compiler Worker：版本化 JSON-lines、同用户 named pipe、
  Frontend/Semantic/Guest IR/WASM 进程内执行、严格请求校验、空闲退出与优雅关闭；
- 详细实现与证据见 [P61.A2a 编译 Worker 协议](P61.A2a_Compiler_Worker_Protocol.md)。
- 完成 `P61.A2b1` Roslyn workspace 复用：同一 Worker 只构建一次 metadata reference set，
  以有界 LRU 复用 primary/reference syntax tree，并通过协议暴露累计指标；
- 详细实现与证据见 [P61.A2b1 Roslyn workspace 复用](P61.A2b1_Roslyn_Workspace_Reuse.md)。
- 完成 `P61.A2b2` Build 链路接入：按 toolchain fingerprint 懒启动同用户 Worker，
  `auto` 传输失败回退 one-shot、`required` fail-closed、`disabled` 保留旧链路；
- 完整 cache hit 不启动 Worker，自定义 Guest compiler 仅将 Frontend/Semantic 放入 Worker；
- 详细实现与证据见 [P61.A2b2 Build 链路接入](P61.A2b2_Build_Worker_Integration.md)。

## 待完成

- `P61.A3` 结构化诊断统一与增量性能矩阵；
- `P61.B` 指令级 source map 与调用栈；
- `P61.C` 断点、单步与变量查看；
- `P61.D` Profiler 与 IDE workspace；
- `P61.E` 集成与集中 Gate。

## 人工验收边界

Automation 可验证协议、映射、状态机和性能预算，但不能代替真实 PIE 暂停/继续、变量检查、源码跳转以及 Visual Studio、Rider、VS Code 的实际打开体验。
