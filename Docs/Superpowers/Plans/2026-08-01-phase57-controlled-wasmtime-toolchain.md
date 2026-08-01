# Phase 57 Controlled Wasmtime Toolchain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立可复现的 Wasmtime v45 AvidScript 工具链，统一启用并身份化 compiler inlining 与 x86-64-v3 配置，然后在冻结 12-kernel 上验证性能领导性。

**Architecture:** 第三方目录只负责 source lock、最小 C API patch 和 managed runtime 构建；`AvidScriptVM` 私有 profile resolver 负责 DLL 扩展发现、CPU 能力、engine 配置和稳定身份。JIT 与 artifact compiler 共享同一 profile，P57.8 的 manifest、attestation 和 runtime authorization 继续作为唯一可信制品链。

**Tech Stack:** Unreal Engine 5.8 C++、Wasmtime 45/Cranelift、Rust/Cargo、C ABI、PowerShell 7、UE Automation、Puerts V8 controlled-runtime harness。

## Global Constraints

- UE 源码目录固定为 `C:\UnrealEngine`，禁止 clean Editor target。
- 不修改冻结 12-kernel、WASM、seed、oracle、进程数、采样数、lane schedule 或 Gate 阈值。
- 不关闭 Spectre mitigation，不启用非标准 fast-math，不用 benchmark 特判。
- 人读文档使用中文；生成依赖、缓存、二进制和日志不得提交。
- 阶段中不做碎片化 UE 构建与正式 benchmark；功能组集成后统一验证。
- 不修改或暂存 `Docs/Phase42/P42.0_Reflection_Binding_Generator_Architecture.md`、`Source/AvidScriptRuntime/Public/AvidScriptWasmReloadTypes.h` 和 `Docs/AvidScript_Technical_Status.md`。

---

### Task 1: 冻结第三方工具链合同

**Files:**
- Create: `Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json`
- Create: `Source/ThirdParty/Wasmtime/PerformanceToolchain/avidscript-wasmtime-v45-inlining.patch`
- Create: `Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1`
- Create: `Build/Contracts/TestWasmtimePerformanceToolchainContracts.ps1`
- Modify: `.gitignore`

**Interfaces:**
- Produces: managed layout `Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0-avidscript.1`。
- Produces: export `avidscript_wasmtime_config_compiler_inlining_set`。
- Consumes: pinned Wasmtime commit `377cd917af258d932d55b201a646917ecf193639`。

- [ ] 固定源码 URL/SHA、patch SHA、Cargo features、Rust version 和输出布局。
- [ ] 补丁只增加 `Inlining::No/Yes` 的 C ABI 映射与头文件声明。
- [ ] Builder 使用内容寻址下载目录和独立 `target` cache，逐项检查 native tool 退出码。
- [ ] 发布前验证 DLL、import library、headers、license、扩展 export 和 managed marker。
- [ ] 合同测试验证 lock schema、路径约束、补丁摘要、命令参数和 `.gitignore` 覆盖。
- [ ] 运行 PowerShell parser 与合同测试，提交 `build(P57.9): add reproducible Wasmtime performance toolchain`。

### Task 2: 建立统一 compiler profile 与 engine factory

**Files:**
- Create: `Source/AvidScriptVM/Private/AvidScriptWasmtimeCompilerProfile.h`
- Create: `Source/AvidScriptVM/Private/AvidScriptWasmtimeCompilerProfile.cpp`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeApi.h`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeApi.c`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeRuntimeSupport.h`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeRuntimeSupport.cpp`
- Modify: `Source/ThirdParty/Wasmtime/Wasmtime.Build.cs`

**Interfaces:**
- Produces: `FAvidScriptWasmtimeCompilerProfile ResolveAvidScriptWasmtimeCompilerProfile(...)`。
- Produces: `avidscript_wasmtime_engine_new_with_profile(const AvidScriptWasmtimeEngineProfile*)`。
- Produces: stable compiler identity and extension function pointer from the verified DLL。

- [ ] 定义固定 profile 字段和无堆分配的 C engine profile POD。
- [ ] 在 Win64 使用 CPUID 验证 x86-64-v3，并把缺失能力返回结构化错误。
- [ ] RuntimeSupport 在 DLL SHA 验证后动态解析扩展，禁止未验证 handle 提供函数指针。
- [ ] C engine factory 应用 strategy、opt、regalloc、inlining、CPU preset、memory 与 semantic flags。
- [ ] Build.cs 优先选择 performance managed layout，官方 layout 只保留诊断 fallback。
- [ ] 运行静态检查与 `git diff --check`，提交 `feat(P57.9): add identity-bound Wasmtime compiler profile`。

### Task 3: 统一 JIT、AOT compiler 与 provenance

**Files:**
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp`
- Modify: `Source/AvidScriptVM/Private/AvidScriptVmArtifactCompiler.cpp`
- Modify: `Source/AvidScriptVM/Private/Tests/AvidScriptVmWasmtimeTests.cpp`
- Modify: `Build/Contracts/TestWasmtimeDependencyContracts.ps1`

**Interfaces:**
- Consumes: Task 2 engine factory and identity。
- Preserves: `CompileAvidScriptVmArtifact`、artifact cache、attestation 和 P57.8 runtime authorization public contracts。

- [ ] JIT backend 和 artifact compiler 只通过共享 profile 创建 engine。
- [ ] compiler identity、cache key、artifact、manifest 和 runtime comparison 使用同一完整字符串。
- [ ] 增加缺扩展、CPU 不兼容、identity mismatch、JIT/serialized 配置一致性测试。
- [ ] 更新依赖合同，证明 performance runtime 与官方 fallback 不会被错误混用。
- [ ] 提交 `feat(P57.9): enforce compiler profile across JIT and serialized artifacts`。

### Task 4: 构建工具链并完成统一 Gate

**Files:**
- Modify: `Benchmarks/PuertsComparison/ControlledRuntime` only when provenance schema needs the new literal identity; frozen workload bytes and measurement settings remain untouched.
- Create: `Docs/Phase57/P57.9_Controlled_Wasmtime_Toolchain.md`
- Modify: `Docs/Phase57/Phase57_State.json`
- Modify: `AGENTS.md` only for mistakes actually observed during execution.

**Interfaces:**
- Consumes: Tasks 1-3 integrated candidate。
- Produces: immutable build, Automation, controlled-runtime and architecture evidence。

- [ ] 在后台构建 performance toolchain；等待期间完成静态合同和 provenance 更新。
- [ ] 对 VM、Runtime、Editor 直接消费者执行一次 UE5.8 no-clean 增量构建。
- [ ] 一次运行 P57.8/P57.9 聚焦 Automation，精确统计 found/started/success/fail/exit。
- [ ] 在冻结 suite 上执行 5 进程正式 shootout，发布同轮 Puerts/Wasmtime aggregate。
- [ ] 若全部阈值通过，将 `P57-D06-ControlledLeadership` 标记 `Verified`；否则保持 `Fixing` 并记录逐 kernel 归因。
- [ ] 在干净候选树运行架构检查，写中文收尾报告与状态快照。
- [ ] 提交 `docs(P57.9): record controlled Wasmtime toolchain evidence`。
