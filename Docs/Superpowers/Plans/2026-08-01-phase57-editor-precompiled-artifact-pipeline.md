# Phase 57 Editor Precompiled Artifact Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 UE 5.8 Win64 Editor 在 C# final build 后自动生成可信 Wasmtime serialized artifact，并让 Runtime 在同一 Editor 会话中自动选择预编译执行、失去 attestation 时安全回退 JIT。

**Architecture:** `AvidScriptVM` 拥有编译器、缓存与进程内 attestation；`AvidScriptEditor` 只负责事务发布和 manifest 可选扩展；`AvidScriptRuntime` 复用旧 manifest loader 验证 canonical WASM，再决定 serialized/JIT lane。所有执行最终进入同一个 VM backend 与生命周期实现。

**Tech Stack:** UE 5.8 C++、Wasmtime 45 C API、Unreal JSON、PowerShell C# guest build、Automation Framework。

## Global Constraints

- 只支持 Win64 Editor producer；非 Win64 返回稳定 `platform_unsupported`。
- 不修改 `Source/AvidScriptRuntime/Public/AvidScriptWasmReloadTypes.h`。
- 不修改 frozen 12-kernel、seed、oracle、operation count 或门禁。
- 不手写 UE API wrapper。
- human-facing 文档使用中文。
- 实现批次中只做静态检查；UE build、Automation、benchmark 在集成后统一执行。
- 保护现有用户改动与未跟踪文档，不暂存、不覆盖、不回滚。

---

### Task 1: VM Artifact Compiler And Session Attestation

**Files:**
- Create: `Source/AvidScriptVM/Public/AvidScriptVmArtifact.h`
- Create: `Source/AvidScriptVM/Private/AvidScriptWasmtimeRuntimeSupport.h`
- Create: `Source/AvidScriptVM/Private/AvidScriptWasmtimeRuntimeSupport.cpp`
- Create: `Source/AvidScriptVM/Private/AvidScriptVmArtifactCompiler.cpp`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp`
- Modify: `Source/AvidScriptVM/Private/Tests/AvidScriptVmWasmtimeTests.cpp`

**Interfaces:**
- Produces:
  - `FAvidScriptVmOwnedArtifact::MakeView(EAvidScriptVmArtifactTrust) const`
  - `bool CompileAvidScriptVmArtifact(const FAvidScriptVmArtifactCompileRequest&, FAvidScriptVmArtifactCompileResult&)`
  - `bool AuthorizeAvidScriptVmArtifact(const FString&, const FAvidScriptVmOwnedArtifact&)`
  - `bool ResolveAvidScriptWasmtimeRuntimeIdentity(FAvidScriptVmBackendInfo&, FString&)`
- Cache key: canonical SHA-256 + backend kind + artifact format + compiler build identity + target triple.
- Registry capacity: 32 artifacts; eviction order is least-recently-used.

- [ ] **Step 1: Add focused compiler/attestation Automation coverage**

Add one `AvidScript.VM.Wasmtime.ArtifactCompiler` test covering real compile, non-empty serialized bytes, exact canonical SHA, 32-char lowercase attestation id, same-process cache hit, exact tuple authorization, and rejection after changing execution SHA/compiler identity/target.

- [ ] **Step 2: Introduce owning artifact and compile result types**

Define the request/result without UE gameplay types. `FAvidScriptVmOwnedArtifact` owns both byte arrays so no `TArrayView` can outlive its producer.

- [ ] **Step 3: Extract one Wasmtime runtime identity owner**

Move DLL load/hash/build-identity production out of the backend anonymous namespace. Backend and compiler must call the same function; no duplicated identity string literals remain.

- [ ] **Step 4: Implement real serialize, LRU cache and attestation registry**

Compile only `Wasmtime + Aot + WasmtimeSerialized`. On cache miss use production engine config, `module_new`, `module_serialize`, then hash and register. On hit return a fresh attestation id bound to the cached immutable bytes.

- [ ] **Step 5: Run scoped static checks and commit**

Run `git diff --check -- Source/AvidScriptVM`, inspect old identity literal count, then commit only Task 1 files. Do not run UBT yet.

---

### Task 2: Editor Transactional Artifact Publisher

**Files:**
- Create: `Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorVmArtifactPublisher.h`
- Create: `Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorVmArtifactPublisher.cpp`
- Modify: `Source/AvidScriptEditor/Public/AvidScriptEditorCSharpBuildService.h`
- Modify: `Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildPipeline.h`
- Modify: `Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildPipeline.cpp`
- Modify: `Source/AvidScriptEditor/Private/Tests/AvidScriptEditorCSharpBuildServiceTests.cpp`
- Modify: `Source/AvidScriptEditor/Private/Tests/AvidScriptEditorCSharpBuildContractTests.cpp`

**Interfaces:**
- Produces `EAvidScriptEditorVmArtifactPolicy { JitOnly, PreferPrecompiled, RequirePrecompiled }`.
- Extends `FAvidScriptEditorCSharpBuildConfig` with `VmArtifactPolicy` defaulting to `PreferPrecompiled`.
- Extends build result with path, format, SHA values, compiler identity, target, attestation id, cache hit, compile milliseconds, fallback category.
- Publisher entry:
  `Publish(const FAvidScriptEditorCSharpBuildConfig&, FAvidScriptEditorCSharpBuildResult&)`.

- [ ] **Step 1: Add publisher and transaction tests**

Cover final-build precompile, bootstrap `JitOnly`, manifest `execution` shape, stale cwasm removal on Prefer failure, and complete rollback on Require failure.

- [ ] **Step 2: Implement atomic cwasm and manifest publication**

Load and parse the already-validated manifest, verify its `wasm.sha256` equals compiler output canonical SHA, write cwasm through a same-directory temporary path, add `execution.policy/fallback`, then atomically replace manifest.

- [ ] **Step 3: Integrate with existing artifact transaction**

Add `<stem>.wasmtime.cwasm` to committed paths. Invoke publisher after final invoker success and before transaction commit. Prefer failures commit canonical WASM without `execution`; Require failures rollback report/manifest/WASM/cwasm together.

- [ ] **Step 4: Expose structured result metadata**

Populate build result without exposing Wasmtime C types or VM-owned byte buffers through the Editor public API.

- [ ] **Step 5: Run scoped static checks and commit**

Run `git diff --check -- Source/AvidScriptEditor`, validate manifest JSON fixtures, and commit Task 2 files only. Do not run UBT yet.

---

### Task 3: Runtime Artifact Loader And Lifecycle Integration

**Files:**
- Create: `Source/AvidScriptRuntime/Public/AvidScriptRuntimeArtifact.h`
- Create: `Source/AvidScriptRuntime/Private/AvidScriptRuntimeArtifact.cpp`
- Modify: `Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h`
- Modify: `Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp`
- Modify: `Source/AvidScriptRuntime/Public/AvidScriptRuntimeSession.h`
- Modify: `Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeSession.cpp`
- Modify: `Source/AvidScriptRuntime/Private/AvidScriptComponent.cpp`
- Modify: `Source/AvidScriptRuntime/Private/Tests/AvidScriptWasmReloadTests.cpp`
- Modify: `Source/AvidScriptRuntime/Private/Tests/AvidScriptRuntimeSessionTests.cpp`
- Modify: `Source/AvidScriptRuntime/Private/Tests/AvidScriptComponentTests.cpp`

**Interfaces:**
- Produces `FAvidScriptRuntimeArtifact` containing the old manifest plus owned VM artifact and selected backend.
- Produces `FAvidScriptRuntimeArtifactLoader::LoadFromFile(...)`.
- Adds `LoadArtifact(...)` overloads to RuntimeInstance and RuntimeSession while preserving every bytecode overload.

- [ ] **Step 1: Add loader fallback and security tests**

Cover verified session artifact, expired/missing attestation JIT fallback, cwasm digest/target/compiler mismatch, Require rejection, and canonical import/layout rejection before any serialized deserialize.

- [ ] **Step 2: Implement loader as a wrapper over the existing manifest loader**

Call old `LoadFromFile` first. Parse optional `execution` only after canonical load succeeds. Resolve relative cwasm within the same allowed project/manifest roots, verify every field and SHA, build an owned artifact, then authorize the tuple.

- [ ] **Step 3: Refactor RuntimeInstance to one shared load implementation**

The old bytecode overload constructs a canonical owned artifact. The new overload passes `MakeView` to backend `LoadArtifact`; binding package preparation, metrics, errors, lifecycle and cleanup remain single-owner.

- [ ] **Step 4: Integrate Session and Component**

Session constructs candidate RuntimeInstance using artifact-selected backend. Component uses the new loader for initial load and reload. Old public bytecode overloads remain source compatible for tests/tools.

- [ ] **Step 5: Run scoped static checks and commit**

Run `git diff --check -- Source/AvidScriptRuntime`, verify the protected reload types file has no staged diff, and commit Task 3 files only. Do not run UBT yet.

---

### Task 4: Unified Gate, Startup Diagnostic And Chinese Evidence

**Files:**
- Modify: `Build/CheckAvidScriptArchitecture.ps1`
- Create: `Docs/Phase57/P57.8_Editor_Precompiled_Artifact_Pipeline.md`
- Modify: `Docs/Phase57/Phase57_State.json`
- Test existing VM, Editor and Runtime Automation files from Tasks 1-3.

- [ ] **Step 1: Perform one concentrated architecture/security review**

Review identity binding, execution/canonical equality, attestation producer, registry bounds, transaction rollback and old API compatibility. Fix Critical/Important findings in one batch.

- [ ] **Step 2: Run one UE 5.8 no-clean Editor build**

Use the repository-standard `AvidTPSTemplateEditor Win64 Development` command. Never clean the Editor target.

- [ ] **Step 3: Run one focused Automation process**

Filter VM artifact compiler, C# build service/contract, Runtime artifact/session/component and architecture VM tests. Parse Started/Success/Fail/Queue Empty/TestExit counts before reporting.

- [ ] **Step 4: Run paired startup diagnostic**

In one process, compile/load the same canonical WASM through JIT and serialized lanes, report cache miss/hit, `RuntimeInitMs`, `ModuleLoadMs`, instantiate time and BeginPlay correctness. Diagnostic target is serialized/JIT `ModuleLoadMs` P50 `<= 0.50`; do not alter steady-state kernels.

- [ ] **Step 5: Commit candidate and run clean architecture check**

Commit only owned changes, move the clean temporary candidate worktree to that commit, and run `Build/CheckAvidScriptArchitecture.ps1` there.

- [ ] **Step 6: Write Chinese evidence and update state honestly**

Record implementation, commands, exact counts, startup result and residual packaged-signature risk. Keep `P57-D06-ControlledLeadership` as `Fixing` unless the unchanged formal controlled-runtime Gate actually passes.
