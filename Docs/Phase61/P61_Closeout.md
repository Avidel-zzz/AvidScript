# Phase 61 收尾记录

状态：实施中，`P61.A1-A3b`、`P61.B1a-B2`、`P61.C1a-C2b1` 已完成

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
- 完成 `P61.A3a` 结构化诊断统一：Frontend/Semantic 使用稳定 code、stage、项目相对
  source id、source SHA-256 与 1-based span，Editor 可安全解析并交给源码访问器；
- 详细实现与证据见 [P61.A3a 结构化诊断与源码定位](P61.A3a_Structured_Diagnostics_And_Navigation.md)。
- 完成 `P61.A3b` 增量性能矩阵：覆盖 no-op、方法体、Binding 与工具链变更，强制验证
  cache 命中、阶段调用次数与耗时预算；
- 5 轮正式矩阵全部通过，四类场景中位数分别为 `883.915 / 1832.748 / 1825.998 /
  3222.949 ms`；
- 详细实现与证据见 [P61.A3b 增量构建性能矩阵](P61.A3b_Incremental_Build_Performance_Matrix.md)。
- 完成 `P61.B1a` DebugMap v2 与 backend offset 回填：同步 CFG 语义点携带稳定身份，
  WASM backend 以真实编码偏移和 WASM hash 原子完成最终 map；
- 详细实现与证据见 [P61.B1a DebugMap v2 与 Backend 偏移回填](P61.B1a_DebugMap_V2_Backend_Offsets.md)。
- 完成 `P61.B1b` Runtime sequence point 映射：v2 map 绑定实际 WASM hash，按函数内 offset
  有界二分查找，同时保留 v1/首点之前的函数级回退；
- 详细实现与证据见 [P61.B1b Runtime Sequence Point 映射](P61.B1b_Runtime_Sequence_Point_Mapping.md)。
- 完成 `P61.B1c` async sequence point：同步 CPS 与 continuation CFG 的 statement、condition、
  await、return 均投影到初始/恢复函数，DataLane Fusion 不再丢失被替换调用的调试位置；
- 详细实现与证据见 [P61.B1c Async Sequence Point](P61.B1c_Async_Sequence_Point_Mapping.md)。
- 完成 `P61.B2` 结构化调用栈：Runtime 区分 C#、WASM、UE host import 与 UE entry，Editor
  Message Log 提供源码 action token，导航前验证项目边界与 source SHA；
- 详细实现与证据见 [P61.B2 Editor 结构化调用栈](P61.B2_Editor_Structured_Call_Stack.md)。
- 完成 `P61.C1a-C1d` 调试 probe 基础闭环：稳定 64 位身份、可选 Guest IR 插桩、正式
  build/worker/cache profile，以及 Wasmtime/WAMR 静态导入和 inactive `Continue` 分发；
- 详细实现与证据见 [P61.C1d VM 调试 Probe 能力](P61.C1d_VM_Debug_Probe_Capability.md)。
- 完成 `P61.C2a` Session debug lane：断点命中、pause-next、continue、step-into 与 runtime
  epoch 失效均由 Session 管理，且不会阻塞 GameThread；
- 详细实现与边界见 [P61.C2a Session 调试通道](P61.C2a_Session_Debug_Lane.md)。
- 完成 `P61.C2b1` 两阶段协作式暂停 frame：只有 Guest spill 提交成功才进入 `Paused`，
  continue/step 通过一次性 token 恢复最多 4 KiB 状态，双后端共享同一 ABI；
- 详细实现与边界见 [P61.C2b1 协作式暂停 Frame](P61.C2b1_Cooperative_Suspension_Frame.md)。

## 待完成

- `P61.C2b2-C4` Guest CPS 恢复、Editor 断点与变量查看；
- `P61.D` Profiler 与 IDE workspace；
- `P61.E` 集成与集中 Gate。

## 人工验收边界

Automation 可验证协议、映射、状态机和性能预算，但不能代替真实 PIE 暂停/继续、变量检查、源码跳转以及 Visual Studio、Rider、VS Code 的实际打开体验。
