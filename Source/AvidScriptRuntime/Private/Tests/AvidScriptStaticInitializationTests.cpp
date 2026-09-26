#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptWasmRuntime.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptStaticInitializationGuardsTest,
    "AvidScript.Runtime.ManagedHeap.StaticInitialization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptStaticInitializationGuardsTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    using namespace AvidScript::Managed;
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (const bool bCooperative : {false, true})
    {
        if (Backend == EAvidScriptVmBackendKind::Wamr && bCooperative) continue;
        const TCHAR* Filename = bCooperative ? TEXT("static-initialization-cooperative.wasm") : TEXT("static-initialization.wasm");
        AddInfo(FString::Printf(TEXT("Static initialization guards: backend=%d cooperative=%d"), static_cast<int32>(Backend), bCooperative));
        TArray<uint8> Wasm;
        if (!TestTrue(TEXT("Read compiler-generated initialization fixture"), FFileHelper::LoadFileToArray(Wasm, *(Directory / Filename)))) return false;
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        FAvidScriptWasmRuntimeInstance Runtime(Selection);
        FAvidScriptWasmSmokeResult Result;
        auto Call = [this, &Runtime](const TCHAR* Name, uint32 Expected)
        {
            FAvidScriptVmPreparedExportCall Prepared;
            FString Error;
            if (!TestTrue(Name, Runtime.PrepareNamedExportCall(Name, Prepared, Error))) { AddError(Error); return false; }
            FAvidScriptVmCallFrame Frame;
            Frame.CellCount = 0;
            FAvidScriptVmCallResult Value;
            FAvidScriptVmError VmError;
            if (!TestTrue(Name, Prepared.Call(Frame, VmError, &Value))) { AddError(VmError.Details); return false; }
            return TestEqual(Name, Value.Cells[0], Expected);
        };
        for (int32 Domain = 0; Domain < 2; ++Domain)
        {
            if (!TestTrue(TEXT("Load fresh initialization domain"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("static-initialization-guards"), Result)))
            { AddError(Result.ErrorMessage); return false; }
            TestEqual(TEXT("Requested initialization backend"), Runtime.GetActiveBackendInfo().Kind, Backend);
            TestEqual(TEXT("Requested initialization execution mode"), Runtime.GetActiveBackendInfo().ExecutionMode, Selection.ExecutionMode);
            if (!TestTrue(TEXT("Begin initialization domain"), Runtime.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
            if (!Call(TEXT("attempts"), 0)) return false;
            FHeap& Heap = *Runtime.GetManagedHeapForTesting();
            for (int32 Iteration = 0; Iteration < 16; ++Iteration)
            {
                if (!Call(TEXT("once"), 1) || !Call(TEXT("cycle"), 32) || !Call(TEXT("reentry"), 7)
                    || !Call(TEXT("generic"), 2) || !Call(TEXT("failure"), 211)
                    || !Call(TEXT("nested_failure"), 221) || !Call(TEXT("attempts"), 1)) return false;
                TestTrue(TEXT("Collect between complete initialization passes"), Heap.Collect() == EHeapError::Ok);
                TestEqual(TEXT("Eight control objects and three error objects remain reachable"), Heap.GetStats().LiveObjects, 11u);
                TestEqual(TEXT("Ready and faulted types never allocate or execute again"), Heap.GetStats().Allocations, uint64(11));
                TestEqual(TEXT("Every closed type retains its own slot plus the observed failure alias"), Heap.GetStats().StaticRoots, 9u);
                TestEqual(TEXT("Only domain roots remain after calls"), Heap.GetStats().LiveRoots, 9u);
                TestEqual(TEXT("Initializer calls release all frames"), Heap.GetStats().ActiveFrames, 0u);
            }
            Runtime.Unload();
            Runtime.Unload();
            TestNull(TEXT("Unload releases all initialization states and cached errors"), Runtime.GetManagedHeapForTesting());
        }
    }
    return true;
}
#endif
