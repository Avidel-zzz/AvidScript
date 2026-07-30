# Phase 57 Adaptive Semantic Reflection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an Adaptive Semantic reflection path that beats Puerts Reflection at the unchanged `<= 0.80x` gate while retaining strict `ProcessEvent` fallback, then clear the five remaining Phase 56 performance gates without weakening evidence.

**Architecture:** Compile eligible reflection descriptors into immutable prepared native invocation plans in `AvidScriptBindings`, adapt those plans into opaque typed host call-sites in `AvidScriptRuntime`, and bind shape-specific Wasmtime callbacks in `AvidScriptVM`. Keep `SemanticProcessEvent` and the generic dynamic bridge as deterministic fallbacks, instrument the actual tier, and finish the independent S1, prepared-export, and controlled-runtime performance axes before one phase-end Gate.

**Tech Stack:** Unreal Engine 5.8 source build, C++20/UE Core containers, Wasmtime 45 C API with Cranelift, C# to WebAssembly guest adapter, PowerShell 7 benchmark/evidence tooling, Unreal Automation.

## Global Constraints

- Engine root is exactly `C:\UnrealEngine`; primary target is Win64 Development Editor.
- `AvidScriptVM` must not expose or depend on UObject, UFunction, FProperty, or project fixture types.
- Do not add per-API handwritten wrappers or branch on benchmark function names.
- Keep strict `SemanticProcessEvent`, generic dynamic dispatch, reload/revoke behavior, object generation checks, host-effect journaling, and failure diagnostics.
- Freeze all six Phase 56 thresholds; do not change workload bytes, seeds, oracle, operation counts, percentile selection, or candidate-binding rules.
- Adaptive reflection must report native hits, strict fallbacks, guard rejections, and logical operation counts separately.
- Generated S1 or Native C++ results must never be used as the adaptive reflection numerator.
- Human-readable design, phase, implementation, and closeout documentation must be Chinese.
- Work in 60–120 minute implementation batches; run the complete UE build, Automation, and formal benchmark only at the phase Gate.
- Do not clean the Editor target.
- Preserve the user's existing modifications in `Docs/Phase42/P42.0_Reflection_Binding_Generator_Architecture.md`, `Source/AvidScriptRuntime/Public/AvidScriptWasmReloadTypes.h`, and `Docs/AvidScript_Technical_Status.md`.

---

## File Structure

### New focused production units

- `Source/AvidScriptBindings/Private/AvidScriptBindingAdaptivePlan.h`
  - Private immutable classifier/plan contract for adaptive reflected calls.
- `Source/AvidScriptBindings/Private/AvidScriptBindingAdaptivePlan.cpp`
  - Native eligibility proof, POD frame proof, native invoke thunk, and strict fallback thunk.
- `Source/AvidScriptRuntime/Private/AvidScriptPreparedReflectionHostCall.h`
  - Runtime-owned prepared call-site context and invalidation token.
- `Source/AvidScriptRuntime/Private/AvidScriptPreparedReflectionHostCall.cpp`
  - Runtime adaptation, callback-epoch receiver validation, host-effect preparation, native/fallback dispatch.

### Existing owners to modify

- `Source/AvidScriptBindings/Public/AvidScriptBindingInvocation.h`
  - Public policy/mode, prepared reflection descriptor, instrumentation, package query.
- `Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp`
  - Build adaptive plan during descriptor load and publish prepared descriptors.
- `Source/AvidScriptVM/Public/AvidScriptVmTypedHostImport.h`
  - VM-neutral prepared adaptive target and typed status.
- `Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp`
  - Bind adaptive typed imports and remove generic per-cell decoding for the supported shape.
- `Source/AvidScriptVM/Private/AvidScriptWasmtimeTypedHostApi.c`
  - Raw Wasmtime shape trampoline and failure-only fallback.
- `Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h`
  - Runtime call-site ownership and callback epoch hooks.
- `Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp`
  - Build/revoke call-sites and expose prepared typed imports.
- `Benchmarks/PuertsComparison/AvidScriptPerfHarness/Source/AvidScriptPerfHarness/Private/AvidScriptPerfRunner.cpp`
  - Adaptive lane, strict diagnostic lane, counters, and result provenance.
- `Benchmarks/PuertsComparison/Schema/*.schema.json`
  - Explicit `adaptive_semantic` lane/mode identities.
- `Benchmarks/PuertsComparison/AvidScriptPerfHarness/Tools/Evaluate-Phase54PerformanceGates.ps1`
  - Keep gate id and threshold; select the explicit adaptive lane as numerator.

---

### Task 1: Bootstrap Phase 57 State And Freeze The Evidence Contract

**Files:**
- Create: `Docs/Phase57/Phase57_State.json` through `Build/InvokePhaseWorkflow.ps1`
- Create: `Docs/Phase57/P57.0_Performance_Contract.md`
- Modify: `Benchmarks/PuertsComparison/AvidScriptPerfHarness/Tests/Test-AvidScriptPerfWarmCore.ps1`
- Modify: `Benchmarks/PuertsComparison/Scripts/Test-PuertsBenchmarkArchitecture.ps1`

**Interfaces:**
- Consumes: Phase 56 compact evidence at `Docs/Phase56/P56.5_Fused_Call_Frame_Benchmark_Evidence.json`
- Produces: tracked phase batches `P57.0` through `P57.5` and static assertions that the six thresholds remain unchanged

- [ ] **Step 1: Write the Chinese frozen-contract document**

Record the exact six Phase 56 values and thresholds, the current raw evidence hashes, the
adaptive lane identity, and the forbidden benchmark mutations from the approved design.

- [ ] **Step 2: Add static contract assertions**

Add assertions equivalent to:

```powershell
Assert-True ($Evaluator.Contains("'semantic_vs_puerts_reflection'")) `
    'the semantic gate id must remain stable'
Assert-True ($Evaluator.Contains('[double]$MaxSemanticOverPuertsReflection = 0.80')) `
    'the semantic/Puerts threshold must remain 0.80'
Assert-True ($Runner.Contains('EAvidScriptBindingInvocationPolicy::AdaptiveSemantic')) `
    'the formal semantic comparator must use the explicit adaptive policy'
Assert-True ($Runner.Contains('EAvidScriptBindingInvocationPolicy::SemanticProcessEvent')) `
    'strict ProcessEvent must remain available as a diagnostic path'
```

- [ ] **Step 3: Run only the static benchmark contracts**

Run:

```powershell
pwsh -NoProfile -File Benchmarks/PuertsComparison/AvidScriptPerfHarness/Tests/Test-AvidScriptPerfWarmCore.ps1
pwsh -NoProfile -File Benchmarks/PuertsComparison/Scripts/Test-PuertsBenchmarkArchitecture.ps1
```

Expected before implementation: the adaptive-policy assertions fail because the enum and lane
do not exist. This is a low-cost contract result, not a reason to build UE.

- [ ] **Step 4: Start the tracked phase**

Run one `Build/InvokePhaseWorkflow.ps1 start` command with:

```text
Phase: 57
ArchitecturePath: Docs/Superpowers/Specs/2026-07-31-phase57-adaptive-semantic-reflection-design.md
PlanPath: Docs/Superpowers/Plans/2026-07-31-phase57-adaptive-semantic-reflection.md
CloseoutPath: Docs/Phase57/P57.5_Adaptive_Semantic_Implementation_Report.md
BatchId: P57.0,P57.1,P57.2,P57.3,P57.4,P57.5
UbtBudget: 3
AutomationBudget: 2
FullGateBudget: 2
```

- [ ] **Step 5: Commit only Task 1 files**

Commit message:

```text
P57.0 freeze adaptive semantic performance contract
```

---

### Task 2: Add The Adaptive Binding Plan

**Files:**
- Create: `Source/AvidScriptBindings/Private/AvidScriptBindingAdaptivePlan.h`
- Create: `Source/AvidScriptBindings/Private/AvidScriptBindingAdaptivePlan.cpp`
- Modify: `Source/AvidScriptBindings/Public/AvidScriptBindingInvocation.h`
- Modify: `Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp`
- Modify: `Source/AvidScriptEditor/Private/Tests/AvidScriptEditorBindingRuntimeIntegrationTests.cpp`

**Interfaces:**
- Consumes: `FAvidScriptRuntimeBindingInvocationPlan`, `FFastPathValueSpec`, descriptor `DispatchMode`, and reflected `UFunction`
- Produces:

```cpp
enum class EAvidScriptBindingInvocationPolicy : uint8
{
    SemanticProcessEvent,
    AdaptiveSemantic,
    QualifiedNativeDirect
};

enum class EAvidScriptBindingInvocationMode : uint8
{
    SemanticProcessEvent,
    AdaptivePreparedNative,
    QualifiedNativeDirect,
    GeneratedNativeS1
};

struct FAvidScriptPreparedReflectionBinding
{
    uint32 BindingOrdinal = MAX_uint32;
    UClass* ExpectedClass = nullptr;
    UFunction* Function = nullptr;
    int32 FrameSize = 0;
    int32 FrameAlignment = 1;
    int32 ParameterOffsets[2] = {};
    int32 ReturnOffset = INDEX_NONE;
    bool bRequiresWriteAccess = false;
};
```

- [ ] **Step 1: Add focused classifier tests**

Extend the existing binding runtime test fixture with:

```cpp
TestTrue(TEXT("Final native POD function is adaptive eligible"),
    Package->TryGetInvocationMode(
        AddBinding->Ordinal,
        EAvidScriptBindingInvocationPolicy::AdaptiveSemantic,
        Mode));
TestEqual(TEXT("Eligible function uses prepared native mode"),
    Mode,
    EAvidScriptBindingInvocationMode::AdaptivePreparedNative);
```

Add negative cases for network, event, Blueprint owner, interface, script bytecode, nontrivial
frame, ref/out, non-final subclass dispatch, and mismatched descriptor identity. Each negative
case must return `SemanticProcessEvent`.

- [ ] **Step 2: Define the private adaptive plan**

Use a focused private contract:

```cpp
namespace UE::AvidScript::BindingPrivate
{
struct FAdaptivePlanBuildSpec
{
    UFunction* Function = nullptr;
    UClass* OwnerClass = nullptr;
    TConstArrayView<FFastPathValueSpec> Parameters;
    FFastPathValueSpec ReturnValue;
    int32 FrameSize = 0;
    int32 FrameAlignment = 1;
    bool bStatic = false;
    bool bRequiresWriteAccess = false;
};

struct FAdaptivePlan
{
    UFunction* Function = nullptr;
    UClass* ExpectedClass = nullptr;
    int32 ParameterOffsets[2] = {};
    int32 ReturnOffset = INDEX_NONE;
    int32 FrameSize = 0;
    int32 FrameAlignment = 1;
    bool bPreparedNativeEligible = false;
};

bool TryBuildAdaptivePlan(
    const FAdaptivePlanBuildSpec& Spec,
    FAdaptivePlan& OutPlan);
}
```

The implementation must check native/final/public flags, all rejected semantic flags, native
function pointer, exact POD property chain, frame links, owner class kind, script bytecode, and
frame size/alignment. It must not inspect function names. The runtime session's explicit
`AdaptiveSemantic` policy is the project-level authorization; the default remains
`SemanticProcessEvent`, so no per-API descriptor allowlist or schema field is added.

- [ ] **Step 3: Publish the policy and instrumentation**

Add package counters:

```cpp
uint64 AdaptivePreparedNativePlanCount = 0;
uint64 AdaptiveStrictFallbackPlanCount = 0;
```

Add warm invocation counters:

```cpp
uint64 AdaptivePreparedNativeHitCount = 0;
uint64 AdaptiveProcessEventFallbackCount = 0;
uint64 AdaptiveGuardRejectCount = 0;
```

- [ ] **Step 4: Build adaptive plans during descriptor load**

Reuse the existing reflected parameters and value plans. Build one adaptive plan per function
binding, store it inside the existing immutable runtime plan, and expose:

```cpp
bool BuildPreparedReflectionBindings(
    TArray<FAvidScriptPreparedReflectionBinding>& OutBindings,
    FString& OutError) const;
```

The method must return only eligible plans and preserve ordinal identity.

- [ ] **Step 5: Run static parser and diff checks**

Run:

```powershell
git diff --check
pwsh -NoProfile -File Benchmarks/PuertsComparison/AvidScriptPerfHarness/Tests/Test-AvidScriptPerfWarmCore.ps1
```

Do not run UBT yet unless a public ABI ambiguity blocks Task 3.

- [ ] **Step 6: Commit the binding plan batch**

Commit message:

```text
P57.1 compile adaptive reflection plans
```

---

### Task 3: Build Runtime Prepared Reflection Call-Sites

**Files:**
- Create: `Source/AvidScriptRuntime/Private/AvidScriptPreparedReflectionHostCall.h`
- Create: `Source/AvidScriptRuntime/Private/AvidScriptPreparedReflectionHostCall.cpp`
- Modify: `Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h`
- Modify: `Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp`
- Modify: `Source/AvidScriptRuntime/Private/Tests/AvidScriptRuntimeSessionTests.cpp`

**Interfaces:**
- Consumes: `FAvidScriptPreparedReflectionBinding`, `FAvidScriptObjectRegistry`, callback epoch, host-effect journal
- Produces: a runtime-owned opaque call-site bound through `FAvidScriptVmPreparedTypedHostTarget`

- [ ] **Step 1: Add prepared call-site lifecycle tests**

Cover:

```text
eligible plan publishes exactly one typed call-site
first call revalidates receiver
second call in same callback epoch is a cache hit
new callback epoch revalidates once
generation change rejects the cached receiver
registry reset rejects the cached receiver
package revoke disables the call-site
successful reload replacement invalidates the old call-site
exact-class mismatch takes strict fallback
```

Assert native hit and strict fallback counters separately.

- [ ] **Step 2: Define the runtime call-site**

Use:

```cpp
struct FAvidScriptPreparedReflectionHostCall
{
    FAvidScriptPreparedReflectionBinding Binding;
    FAvidScriptVmTypedHostImport Import;
    FAvidScriptWasmRuntimeInstance* Runtime = nullptr;
    uint64 PackageRevision = 0;
};
```

Keep UObject-bearing state private to Runtime. Do not place this struct in the VM module.

- [ ] **Step 3: Add callback-epoch receiver validation**

The fast frame must store:

```cpp
struct FAvidScriptAdaptiveReceiverFrame
{
    uint64 CallbackEpoch = 0;
    uint64 RegistryRevision = 0;
    FAvidScriptObjectHandle Handle;
    TWeakObjectPtr<UObject> Object;
    UClass* ExpectedClass = nullptr;
};
```

Reuse existing fused callback epoch entry/exit. Cache only inside the active callback and check
slot, generation, registry revision, expected class, object validity, and package revision.

- [ ] **Step 4: Implement prepared native and strict fallback dispatch**

Expose a shape-specific function:

```cpp
static EAvidScriptVmTypedHostStatus InvokePreparedReflectionSelfI32Pair(
    void* Context,
    int32 SelfSlot,
    int32 SelfGeneration,
    int32 Left,
    int32 Right,
    int32& OutValue);
```

The prepared native branch must create the proven POD frame, construct `FFrame`, call
`UFunction::Invoke`, and return the int32 result. The fallback branch must call the existing
strict ProcessEvent thunk for the same ordinal and arguments.

- [ ] **Step 5: Integrate call-site build/revoke**

Build prepared reflection imports beside generated imports after the binding package is loaded.
Reset them before backend unload, package replacement, host-context replacement, and runtime
destruction. Never leave a VM import context pointing to freed Runtime memory.

- [ ] **Step 6: Run low-cost checks and commit**

Run `git diff --check` and the static benchmark contracts. Commit:

```text
P57.1 add prepared reflection call sites
```

---

### Task 4: Add The Wasmtime Adaptive Typed Bridge

**Files:**
- Modify: `Source/AvidScriptVM/Public/AvidScriptVmTypedHostImport.h`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeTypedHostApi.c`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeTypedHostApi.h`
- Modify: `Source/AvidScriptVM/Private/Tests/AvidScriptVmWasmtimeTests.cpp`

**Interfaces:**
- Consumes: `FAvidScriptVmTypedHostImport::PreparedTarget`
- Produces: a fixed `SelfI32PairToI32` Wasmtime callback that can return success, rejection, or strict fallback

- [ ] **Step 1: Add VM-neutral bridge tests**

Test one import for each result:

```text
Succeeded: returns exact int32 and never calls generic dispatcher
FallbackRequired: calls the supplied fallback target exactly once
Rejected: reports host_function_failed without invoking fallback
trap/reentrant unload: preserves existing deferred unload behavior
```

- [ ] **Step 2: Extend the prepared target contract**

Do not add UE types. Add a distinct target slot only if the existing `SelfI32Pair` target cannot
carry the adaptive fallback contract. The preferred shape is:

```cpp
FAvidScriptVmPreparedSelfI32PairTarget SelfI32Pair = nullptr;
FAvidScriptVmPreparedSelfI32PairTarget SelfI32PairFallback = nullptr;
```

The raw trampoline calls `SelfI32Pair` first and calls `SelfI32PairFallback` only when status is
`FallbackRequired`.

- [ ] **Step 3: Bind the shape at module load**

Parse and validate the compact signature once during import linking. Store the prepared target in
the permanent host context. The callback must directly read four i32 parameters and write one i32
result without the generic `uint64 ArgumentCells[64]` loop.

- [ ] **Step 4: Keep diagnostics on the failure path**

Successful calls must not reset or allocate `FString`. Rejection and trap paths retain category
and details through the existing VM error channel.

- [ ] **Step 5: Run one ABI-risk focused build**

Because this task changes the public VM typed-host ABI, run one no-clean scoped build after Tasks
2–4 are integrated:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat" AvidTPSTemplateEditor Win64 Development "-Project=C:\Users\12159\Documents\Unreal Projects\AvidTPSTemplate\AvidTPSTemplate.uproject" -WaitMutex -NoHotReloadFromIDE -Module=AvidScriptVM -Module=AvidScriptBindings -Module=AvidScriptRuntime -Module=AvidScriptEditor
```

Redirect full output to `C:\tmp\AvidScript_P57_ABI_Build.log` and report only exit code and
AvidScript errors. This is the one permitted midpoint build for the cross-module ABI change.

- [ ] **Step 6: Commit the typed bridge**

Commit message:

```text
P57.2 bind adaptive reflection through typed Wasmtime calls
```

---

### Task 5: Make The Benchmark Comparison Explicit And Fair

**Files:**
- Modify: `Benchmarks/PuertsComparison/Schema/BenchmarkResult.schema.json`
- Modify: `Benchmarks/PuertsComparison/Schema/BenchmarkProcessResult.schema.json`
- Modify: `Benchmarks/PuertsComparison/Schema/BenchmarkProcessRequest.schema.json`
- Modify: `Benchmarks/PuertsComparison/Schema/BenchmarkAggregate.schema.json`
- Modify: `Benchmarks/PuertsComparison/Schema/BenchmarkCalibration.schema.json`
- Modify: `Benchmarks/PuertsComparison/Config/BenchmarkProfile.json`
- Modify: `Benchmarks/PuertsComparison/AvidScriptPerfHarness/Source/AvidScriptPerfHarness/Private/AvidScriptPerfRunner.cpp`
- Modify: `Benchmarks/PuertsComparison/AvidScriptPerfHarness/Tools/Evaluate-Phase54PerformanceGates.ps1`
- Modify: `Benchmarks/PuertsComparison/AvidScriptPerfHarness/Tests/Test-AvidScriptPerfWarmCore.ps1`
- Modify: `Benchmarks/PuertsComparison/Scripts/Test-PuertsBenchmarkArchitecture.ps1`

**Interfaces:**
- Consumes: adaptive policy/mode and instrumentation from Tasks 2–4
- Produces: explicit `avidscript_wasmtime_adaptive_semantic` lane and unchanged `semantic_vs_puerts_reflection` gate

- [ ] **Step 1: Add explicit schema identities**

Add:

```json
{
  "lane_id": "avidscript_wasmtime_adaptive_semantic",
  "binding_invocation_mode": "adaptive_semantic"
}
```

Keep the old `avidscript_wasmtime_semantic` / `semantic_process_event` identity available for
strict diagnostics.

- [ ] **Step 2: Publish actual tier counters**

Each adaptive sample must include:

```text
adaptive_native_hit_count
adaptive_process_event_fallback_count
adaptive_guard_reject_count
semantic_process_event_count
generated_s1_hit_count
logical_operation_count
host_crossing_count
```

For the scalar formal lane:

```text
adaptive_native_hit_count == logical_operation_count
adaptive_process_event_fallback_count == 0
generated_s1_hit_count == 0
```

- [ ] **Step 3: Keep the gate threshold unchanged**

Change only the numerator lane lookup:

```powershell
$AdaptiveSemantic = Get-LaneMetric `
    -LaneId 'avidscript_wasmtime_adaptive_semantic' `
    -WorkloadId 'scalar_add_int32'
$SemanticOverPuerts = $AdaptiveSemantic / $PuertsReflection
```

The existing maximum remains exactly `0.80`.

- [ ] **Step 4: Add equal-iteration diagnostic evidence**

Add a diagnostic-only profile that runs AvidScript adaptive and Puerts reflection at the same
`8k`, `64k`, and `256k` logical operation counts. Do not replace the frozen formal profile.
Report the slope/intercept sensitivity so outer dispatch amortization is visible.

- [ ] **Step 5: Run static contracts**

Run all benchmark schema/parser/architecture harnesses. Expected: all static contracts pass
without launching UE.

- [ ] **Step 6: Commit benchmark identities**

Commit message:

```text
P57.2 publish honest adaptive reflection benchmarks
```

---

### Task 6: Clear Generated S1 Scalar And Property Gates

**Files:**
- Modify: `Source/AvidScriptVM/Public/AvidScriptVmTypedHostImport.h`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeTypedHostApi.c`
- Modify: `Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp`
- Modify: `Source/AvidScriptRuntime/Private/Tests/AvidScriptRuntimeSessionTests.cpp`
- Modify: `Benchmarks/PuertsComparison/AvidScriptPerfHarness/Source/AvidScriptPerfHarness/Private/AvidScriptPerfRunner.cpp`

**Interfaces:**
- Consumes: existing prepared generated binding and fused callback frame
- Produces: a stable success-path call cell shared by scalar and property typed imports

- [ ] **Step 1: Add diagnostic segment counters**

Behind `EAvidScriptDynamicHostCallTimingPolicy::PerCall`, accumulate:

```text
raw_typed_trampoline_cycles
prepared_target_cycles
receiver_effect_cycles
generated_thunk_cycles
result_propagation_cycles
```

Production default performs integer hit counting only.

- [ ] **Step 2: Remove repeated success-path checks**

At import link time validate shape and target once. Store a call cell containing context, target,
fallback, and revision token. Normal calls check one token, then invoke the function pointer.
Move error container reset and details construction to failure paths.

- [ ] **Step 3: Reuse receiver/effect validation**

Property set/get calls within one callback reuse the callback-epoch receiver frame. Write access
and journal mode must be prepared once per call-site per epoch when the effect contract permits;
journaled writes still prepare each distinct reversible effect.

- [ ] **Step 4: Run diagnostic micro benchmark**

Run only the five-process diagnostic micro profile after the shared call-cell implementation.
Go criteria:

```text
scalar <= 24.5 ns
property <= 49.0 ns
typed empty net <= 4.5 ns
callback regression <= 5%
correct observations == total observations
```

If the shared segment saves less than `2.0 ns/call`, keep the diagnostic evidence and do not add
another cache layer without a measured owner.

- [ ] **Step 5: Commit the S1 crossing batch**

Commit message:

```text
P57.3 slim prepared typed host call cells
```

---

### Task 7: Clear The Prepared Export Gate

**Files:**
- Modify: `Source/AvidScriptVM/Public/AvidScriptVmBackend.h`
- Modify: `Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp`
- Modify: `Source/AvidScriptVM/Private/Tests/AvidScriptVmWasmtimeTests.cpp`
- Modify: `Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp`
- Modify: `Source/AvidScriptRuntime/Private/Tests/AvidScriptRuntimeSessionTests.cpp`

**Interfaces:**
- Consumes: `FAvidScriptVmPreparedExportCall`
- Produces: stable prepared export cell with one validation token and failure-only diagnostics

- [ ] **Step 1: Add lifecycle and failure tests**

Cover exact result, wrong cell count, stale target, trap, host failure, reentrant unload, deferred
unload, tombstone, and successful reload replacement.

- [ ] **Step 2: Converge Runtime and benchmark entry points**

Both product lifecycle dispatch and physical gradient benchmark must invoke:

```cpp
PreparedCall.Call(Frame, OutError, &OutResult);
```

Do not expose or benchmark an unchecked helper that Runtime does not use.

- [ ] **Step 3: Make validation token-based**

Resolve target, parameter count, result count, store/context identity, and generation at prepare
time. The call path checks cell counts and one active generation token before invoking the cached
function pointer. Failure paths materialize `FAvidScriptVmError`.

- [ ] **Step 4: Run the physical-cost diagnostic**

Go criteria across five processes:

```text
prepared_over_generic <= 0.94
paired savings >= 1.5 ns
all trap and unload cases pass
```

- [ ] **Step 5: Commit the export batch**

Commit message:

```text
P57.4 reduce prepared export call overhead
```

---

### Task 8: Clear Wasmtime/V8 Controlled Runtime Gates

**Files:**
- Modify only when evidence identifies an owner:
  - `Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp`
  - `Source/AvidScriptVM/Private/AvidScriptWasmtimeRuntimeApi.h`
  - `Benchmarks/PuertsComparison/ControlledRuntime/Tools/Invoke-ControlledRuntimeSuite.ps1`
- Create: `Docs/Phase57/P57.4_Controlled_Runtime_Optimization_Report.md`

**Interfaces:**
- Consumes: the frozen 12-kernel controlled-runtime suite
- Produces: Wasmtime configuration/code-path changes that improve the unchanged P95 gates

- [ ] **Step 1: Run codegen and jitter attribution**

Collect generated-code/configuration evidence for the six Phase 56 P95 losses:

```text
hashing
internal_call
parallel_integer
predictable_branch
sequential_memory
simd128
```

Compare affinity, epoch interruption, fuel, pooling/allocation strategy, Cranelift optimization
level, NaN canonicalization, guard pages, and host timer placement. Do not alter WAT/WASM bytes.

- [ ] **Step 2: Apply only product-valid Wasmtime settings**

Each retained change must be used by normal `CreateAvidScriptWasmtimeBackend`, preserve traps,
memory safety, interruption, and unload behavior, and improve at least two independent kernels or
the suite geomean. Benchmark-only flags are rejected.

- [ ] **Step 3: Run controlled diagnostic**

Go criteria:

```text
P95 geomean <= 0.94
P95 wins >= 8/12
each kernel ratio <= 1.05
mixed_gameplay regression <= 2%
```

Record all attempted settings, including reverted no-gain changes, in the Chinese report.

- [ ] **Step 4: Commit retained backend changes**

Commit message:

```text
P57.4 tune Wasmtime controlled runtime performance
```

---

### Task 9: Central Review, Final Gate, Closeout, And Push

**Files:**
- Create: `Docs/Phase57/P57.5_Adaptive_Semantic_Implementation_Report.md`
- Create: `Docs/Phase57/P57.5_Adaptive_Semantic_Benchmark_Evidence.json`
- Modify: `Docs/Phase57/Phase57_State.json` only through workflow commands
- Modify: `README.md` only after formal evidence exists

**Interfaces:**
- Consumes: all Phase 57 batches and a clean candidate commit
- Produces: immutable formal evidence, gate attestation, Chinese closeout, pushed `main`

- [ ] **Step 1: Run one centralized code and architecture review**

Review exact-class/final proof, event/RPC/interface fallback, callback reentry, object and package
invalidation, host-effect journaling, VM/UE module boundary, benchmark provenance, and all changed
public ABI. Produce one findings list and one consolidated fix batch.

- [ ] **Step 2: Run focused fixes**

Run only tests directly affected by review fixes. Do not start the full Automation until the
candidate is ready.

- [ ] **Step 3: Complete tracked batches and freeze**

Mark `P57.0` through `P57.4` complete with exact commit/test evidence, write the Chinese closeout
draft, commit product and docs, then run `freeze` to create the `gate_ready` candidate state.

- [ ] **Step 4: Run the phase-end Gate**

Run:

```text
all .NET harnesses and formatting
one four-module no-clean UE5.8 Development Editor build
architecture/parser/static contracts
one complete Automation RunTests AvidScript
diagnostic benchmarks
5-process formal micro/gameplay/controlled-runtime benchmarks
```

Redirect UE/UBT output to `C:\tmp` and parse performed, success, fail, Queue Empty, TestExit, and
process exit before reporting success.

- [ ] **Step 5: Evaluate immutable evidence**

Require:

```text
all six transferred gates pass
formal adaptive/Puerts ratio <= 0.80
adaptive native hits == expected scalar logical operations
adaptive fallback == 0 for the eligible scalar formal lane
strict fallback tests pass for every ineligible semantic category
Automation found == completed == succeeded >= 317
failed == 0
candidate commit/tree clean and evidence-bound
```

- [ ] **Step 6: Attest and close**

Run workflow `attest` with the formal report, commit the allowed attestation change, produce close
evidence, and run `close`. Do not call Phase 57 complete if any threshold remains red.

- [ ] **Step 7: Update public README from formal evidence**

Replace the Phase 56 semantic gap only with measured Phase 57 values. Describe Adaptive Semantic,
strict ProcessEvent fallback, actual coverage, environment, and limitations in Chinese/English
consistent with the existing README structure.

- [ ] **Step 8: Push without protected user files**

Verify staged paths, confirm the three protected user files are neither staged nor modified by
Phase 57, push `main`, and verify `refs/heads/main` matches local HEAD.
