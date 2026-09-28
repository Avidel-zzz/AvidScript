#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptLanguageErrorCatalog.h"
#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptStaticSourceInitializationTest,
    "AvidScript.Runtime.ManagedHeap.StaticSourceInitialization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptStaticSourceInitializationTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    using namespace AvidScript::Managed;
    // The managed fixture producer executes the same source with .NET before
    // emitting these modules, including GC after every allocation/static write.
    struct FCase { const TCHAR* Name; uint32 First; uint32 Second; uint32 Roots; bool bObjects; };
    const FCase Cases[] = {
        {TEXT("objects"), 123, 123, 4, true}, {TEXT("cycle"), 32, 32, 2, false},
        {TEXT("reentry"), 7, 7, 1, false}, {TEXT("generic"), 12, 12, 3, false},
        {TEXT("construct"), 1324, 132424, 2, false}, {TEXT("store-order"), 12, 121, 2, false},
        {TEXT("call-order"), 123, 12313, 2, false}, {TEXT("defaults"), 12, 24, 2, false},
        {TEXT("ref-order"), 21306, 2131307, 3, false}, {TEXT("struct-call"), 23, 23, 2, false},
    };
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (const bool bCooperative : {false, true})
    {
        if (Backend == EAvidScriptVmBackendKind::Wamr && bCooperative) continue;
        for (const FCase& Case : Cases)
        {
            AddInfo(FString::Printf(TEXT("Static source: case=%s backend=%d cooperative=%d"), Case.Name, static_cast<int32>(Backend), bCooperative));
            const FString Filename = FString::Printf(TEXT("static-source-%s%s.wasm"), Case.Name, bCooperative ? TEXT("-cooperative") : TEXT(""));
            const FString ModuleId = FString::Printf(TEXT("csharp:Scripts/StaticSource-%s.cs"), Case.Name);
            TArray<uint8> Wasm;
            if (!TestTrue(TEXT("Read C# source-generated fixture"), FFileHelper::LoadFileToArray(Wasm, *(Directory / Filename)))) return false;
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
                if (!TestTrue(TEXT("Load fresh C# static domain"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, Result)))
                { AddError(Result.ErrorMessage); return false; }
                TestEqual(TEXT("C# static source backend"), Runtime.GetActiveBackendInfo().Kind, Backend);
                TestEqual(TEXT("C# static source mode"), Runtime.GetActiveBackendInfo().ExecutionMode, Selection.ExecutionMode);
                if (!TestTrue(TEXT("Begin C# static domain"), Runtime.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
                if (!Call(TEXT("run"), Case.First)) return false;
                FHeap* Heap = Runtime.GetManagedHeapForTesting();
                if (!TestNotNull(TEXT("Static source owns a managed heap"), Heap)) return false;
                TestTrue(TEXT("Collect between source calls"), Heap->Collect() == EHeapError::Ok);
                if (!Call(TEXT("run"), Case.Second)) return false;
                if (Case.bObjects)
                    for (int32 Iteration = 0; Iteration < 16; ++Iteration)
                    {
                        if (!Call(TEXT("replace"), 19) || !Call(TEXT("run"), 123)) return false;
                        TestTrue(TEXT("Collect replaced objects while preserving static aliases"), Heap->Collect() == EHeapError::Ok);
                        TestEqual(TEXT("Only two control objects and two source objects survive"), Heap->GetStats().LiveObjects, 4u);
                    }
                TestTrue(TEXT("Collect after source execution"), Heap->Collect() == EHeapError::Ok);
                TestEqual(TEXT("Source static roots survive calls"), Heap->GetStats().StaticRoots, Case.Roots);
                TestEqual(TEXT("Only domain roots survive calls"), Heap->GetStats().LiveRoots, Case.Roots);
                TestEqual(TEXT("Initializer and source frames are released"), Heap->GetStats().ActiveFrames, 0u);
                TestEqual(TEXT("Temporary source objects are reclaimed"), Heap->GetStats().LiveObjects, Case.Roots);
                Runtime.Unload();
                Runtime.Unload();
                TestNull(TEXT("Source domain unload releases heap and initialization state"), Runtime.GetManagedHeapForTesting());
            }
        }
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptComposableIr35StaticTokenTest,
    "AvidScript.Runtime.ManagedHeap.ComposableIr35StaticToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComposableIr35StaticTokenTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    using namespace AvidScript::Managed;
    const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("AvidScriptComposableIr35/GuestFixtures/composable-static-token.wasm"));
    TArray<uint8> Wasm;
    if (!TestTrue(TEXT("Read same-source IR 35 WASM fixture"), FFileHelper::LoadFileToArray(Wasm, *Path))) return false;

    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    {
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
            ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        FAvidScriptWasmRuntimeInstance Runtime(Selection);
        for (int32 Domain = 0; Domain < 2; ++Domain)
        {
            FAvidScriptWasmSmokeResult Result;
            if (!TestTrue(TEXT("Load canonical IR 35 module"), Runtime.LoadModule(
                Wasm.GetData(), Wasm.Num(), TEXT("csharp:Scripts/ComposableStaticToken.cs"), Result)))
            { AddError(Result.ErrorMessage); return false; }
            TestEqual(TEXT("IR 35 uses requested backend"), Runtime.GetActiveBackendInfo().Kind, Backend);
            TestEqual(TEXT("IR 35 uses requested execution mode"),
                Runtime.GetActiveBackendInfo().ExecutionMode, Selection.ExecutionMode);
            if (!TestTrue(TEXT("Begin IR 35 static domain"), Runtime.BeginPlay(Result)))
            { AddError(Result.ErrorMessage); return false; }
            FAvidScriptVmPreparedExportCall Prepared;
            FString Error;
            if (!TestTrue(TEXT("Prepare IR 35 run export"), Runtime.PrepareNamedExportCall(
                TEXT("run"), Prepared, Error)))
            { AddError(Error); return false; }
            for (const uint32 Expected : {2u, 3u})
            {
                FAvidScriptVmCallFrame Frame;
                Frame.CellCount = 0;
                FAvidScriptVmCallResult Value;
                FAvidScriptVmError VmError;
                if (!TestTrue(TEXT("Execute combined static/token source"), Prepared.Call(Frame, VmError, &Value)))
                { AddError(VmError.Details); return false; }
                if (!TestEqual(TEXT("Static state and token equality match source"), Value.Cells[0], Expected)) return false;
            }
            FHeap* Heap = Runtime.GetManagedHeapForTesting();
            if (!TestNotNull(TEXT("IR 35 static domain owns a heap"), Heap)) return false;
            TestTrue(TEXT("Collect after combined execution"), Heap->Collect() == EHeapError::Ok);
            TestEqual(TEXT("Combined source leaves no active frames"), Heap->GetStats().ActiveFrames, 0u);
            TestEqual(TEXT("Only domain roots remain"), Heap->GetStats().LiveRoots, Heap->GetStats().StaticRoots);
            UE_LOG(LogTemp, Display, TEXT("composable-ir35 backend=%d domain=%d first=2 second=3 roots=%u"),
                static_cast<int32>(Backend), Domain, Heap->GetStats().StaticRoots);
            TestTrue(TEXT("End IR 35 static domain"), Runtime.EndPlay(Result));
            Runtime.Unload();
            TestNull(TEXT("IR 35 unload releases static heap"), Runtime.GetManagedHeapForTesting());
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptComposableIr35AsyncExecutionTest,
    "AvidScript.Runtime.ManagedHeap.ComposableIr35AsyncExecution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComposableIr35AsyncExecutionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (!TestNotNull(TEXT("Create async IR 35 test World with GEngine"), GEngine)) return false;
    const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("AvidScriptComposableIr35/GuestFixtures/composable-async-static-token.wasm"));
    TArray<uint8> Wasm;
    if (!TestTrue(TEXT("Read same-source async IR 35 WASM fixture"), FFileHelper::LoadFileToArray(Wasm, *Path))) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptComposableIr35World"));
    if (!TestNotNull(TEXT("Async IR 35 test World created"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (int32 Scenario = 0; Scenario < 3; ++Scenario)
    {
        const bool bCancelBeforeTick = Scenario == 1;
        const bool bTeardownBeforeTick = Scenario == 2;
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
            ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        FAvidScriptWasmRuntimeInstance Runtime(Selection);
        FAvidScriptWasmSmokeResult Result;
        if (!TestTrue(TEXT("Load same-source async IR 35 module"), Runtime.LoadModule(
            Wasm.GetData(), Wasm.Num(), TEXT("csharp:Scripts/ComposableAsyncStaticToken.cs"), Result)))
        { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Async IR 35 uses requested backend"), Runtime.GetActiveBackendInfo().Kind, Backend);
        const FAvidScriptLanguageErrorCatalog* Catalog = Runtime.GetLanguageErrorCatalog();
        TestTrue(TEXT("Async IR 35 authorizes fault, cancellation identity and exception token"), Catalog
            && Catalog->SupportsTaskLanguageErrorFault() && Catalog->SupportsTaskCancellationError()
            && Catalog->SupportsTaskCancellationIdentity() && Catalog->SupportsExceptionCancellationToken());
        if (!TestTrue(TEXT("Async IR 35 exports entry, resumer, cancellation and result"), Runtime.ValidateRequiredExports(
            {TEXT("avid_on_begin_play"), TEXT("avid_on_continuation_v2"), TEXT("avid_on_end_play"),
                TEXT("avid_cancel"), TEXT("avid_get_result")}, Result)))
        { AddError(Result.ErrorMessage); return false; }
        const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
        auto& Endpoint = Owner->ResetActive(World);
        FAvidScriptWasmHostContext Context;
        Context.Tasks = &Endpoint;
        Context.Continuations = &Endpoint;
        Context.World = World;
        Runtime.SetHostContext(Context);
        ON_SCOPE_EXIT { Runtime.SetHostContext({}); Owner->Teardown(); Runtime.Unload(); };
        FAvidScriptVmPreparedExportCall ResultCall;
        FString Error;
        if (!TestTrue(TEXT("Prepare async IR 35 result export"),
            Runtime.PrepareNamedExportCall(TEXT("avid_get_result"), ResultCall, Error)))
        { AddError(Error); return false; }
        auto ReadResult = [&]() -> int32
        {
            FAvidScriptVmCallFrame Frame;
            Frame.CellCount = 0;
            FAvidScriptVmCallResult Value;
            FAvidScriptVmError VmError;
            if (!ResultCall.Call(Frame, VmError, &Value))
            { AddError(VmError.Details); return MIN_int32; }
            return static_cast<int32>(Value.Cells[0]);
        };
        if (!TestTrue(TEXT("BeginPlay suspends async IR 35 source"), Runtime.BeginPlay(Result)))
        { AddError(Result.ErrorMessage); return false; }
        const int32 PendingResult = ReadResult();
        TestEqual(TEXT("Async result waits for the next tick"), PendingResult, 0);
        TestTrue(TEXT("Async source registered a continuation"), Owner->GetActiveCount() > 0);
        if (bTeardownBeforeTick)
        {
            if (!TestTrue(TEXT("End async IR 35 domain while suspended"), Runtime.EndPlay(Result)))
            { AddError(Result.ErrorMessage); return false; }
            Owner->Teardown();
            TArray<FAvidScriptContinuationCompletion> Ready;
            Owner->DrainReady(Ready);
            const int32 RemainingContinuations = Owner->GetActiveCount();
            const int32 RemainingTasks = Owner->GetTaskResultsForTesting().GetCount();
            const int32 RemainingSources = Owner->GetCancellationSourceCountForTesting();
            TestEqual(TEXT("Suspended IR 35 teardown discards ready callbacks"), Ready.Num(), 0);
            TestEqual(TEXT("Suspended IR 35 teardown releases continuations"), RemainingContinuations, 0);
            TestEqual(TEXT("Suspended IR 35 teardown releases task results"), RemainingTasks, 0);
            TestEqual(TEXT("Suspended IR 35 teardown releases cancellation sources"), RemainingSources, 0);
            TestTrue(TEXT("Suspended IR 35 teardown releases invocation frames"),
                Runtime.GetManagedHeapForTesting()
                    && Runtime.GetManagedHeapForTesting()->GetStats().ActiveFrames == 0);
            UE_LOG(LogTemp, Display, TEXT("composable-ir35-async backend=%d mode=teardown result=%d continuations=%d tasks=%d sources=%d"),
                static_cast<int32>(Backend), PendingResult, RemainingContinuations, RemainingTasks, RemainingSources);
            Runtime.Unload();
            TestNull(TEXT("Suspended async IR 35 unload releases heap"), Runtime.GetManagedHeapForTesting());
            continue;
        }
        if (bCancelBeforeTick)
        {
            FAvidScriptVmPreparedExportCall CancelCall;
            if (!TestTrue(TEXT("Prepare async IR 35 cancellation export"),
                Runtime.PrepareNamedExportCall(TEXT("avid_cancel"), CancelCall, Error)))
            { AddError(Error); return false; }
            FAvidScriptVmCallFrame Frame;
            Frame.CellCount = 0;
            FAvidScriptVmCallResult Value;
            FAvidScriptVmError VmError;
            if (!TestTrue(TEXT("Cancel async IR 35 source before the next tick"),
                CancelCall.Call(Frame, VmError, &Value)))
            { AddError(VmError.Details); return false; }
        }
        for (int32 Round = 0; Round < 4 && Owner->GetActiveCount() > 0; ++Round)
        {
            if (!bCancelBeforeTick)
            {
                World->Tick(LEVELTICK_All, 0.02f);
                ++GFrameCounter;
            }
            TArray<FAvidScriptContinuationCompletion> Ready;
            Owner->DrainReady(Ready);
            for (const auto& Completion : Ready)
            {
                const bool bDispatched = Runtime.DispatchContinuation(Completion, Result);
                if (!TestTrue(TEXT("Async IR 35 continuation resumes"), bDispatched))
                { AddError(Result.ErrorMessage); return false; }
                if (!TestTrue(TEXT("Async IR 35 continuation finalizes"),
                    Owner->FinalizeDispatched(Completion.Token, bDispatched))) return false;
            }
        }
        const int32 CompletedResult = ReadResult();
        const int32 RemainingContinuations = Owner->GetActiveCount();
        TestEqual(TEXT("Async IR 35 completes or catches cancellation with matching token"),
            CompletedResult, bCancelBeforeTick ? 7 : 1);
        TestEqual(TEXT("Async IR 35 releases its continuations"), RemainingContinuations, 0);
        TestTrue(TEXT("Async IR 35 releases invocation frames"),
            Runtime.GetManagedHeapForTesting()
                && Runtime.GetManagedHeapForTesting()->GetStats().ActiveFrames == 0);
        if (!TestTrue(TEXT("End async IR 35 domain"), Runtime.EndPlay(Result)))
        { AddError(Result.ErrorMessage); return false; }
        const int32 RemainingTasks = Owner->GetTaskResultsForTesting().GetCount();
        const int32 RemainingSources = Owner->GetCancellationSourceCountForTesting();
        TestEqual(TEXT("Async IR 35 releases task results"), RemainingTasks, 0);
        TestEqual(TEXT("Async IR 35 releases cancellation sources"), RemainingSources, 0);
        UE_LOG(LogTemp, Display, TEXT("composable-ir35-async backend=%d mode=%s result=%d continuations=%d tasks=%d sources=%d"),
            static_cast<int32>(Backend), bCancelBeforeTick ? TEXT("cancel") : TEXT("complete"),
            CompletedResult, RemainingContinuations, RemainingTasks, RemainingSources);
        Runtime.Unload();
        TestNull(TEXT("Async IR 35 unload releases heap"), Runtime.GetManagedHeapForTesting());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptStaticSourceFailuresTest,
    "AvidScript.Runtime.ManagedHeap.StaticSourceFailures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptStaticSourceFailuresTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    using namespace AvidScript::Managed;
    struct FCase { const TCHAR* Name; uint32 First; uint32 Increment; uint32 Roots; uint32 Objects; };
    // First and second values are independently checked by executing the same C#
    // with .NET. Repeated calls then exercise cached identity and cleanup effects.
    const FCase Cases[] = {
        {TEXT("field"), 11, 0, 2, 4}, {TEXT("cctor"), 12, 0, 2, 4},
        {TEXT("nested"), 13, 0, 3, 6}, {TEXT("identity"), 14, 0, 4, 5},
        {TEXT("generic"), 20, 0, 3, 7}, {TEXT("finally"), 11, 10, 3, 5},
        {TEXT("finally-call"), 11, 10, 2, 4}, {TEXT("finally-normal"), 10, 10, 1, 1},
        {TEXT("nested-finally-call"), 1101, 1001, 3, 5}, {TEXT("finally-static"), 111, 0, 3, 7},
        {TEXT("rethrow"), 15, 0, 2, 4}, {TEXT("caught-inside"), 71, 0, 2, 2},
        {TEXT("published-alias"), 71, 0, 5, 6},
    };
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (const bool bCooperative : {false, true})
    {
        if (Backend == EAvidScriptVmBackendKind::Wamr && bCooperative) continue;
        for (const FCase& Case : Cases)
        {
            AddInfo(FString::Printf(TEXT("Static source failure: case=%s backend=%d cooperative=%d"), Case.Name, static_cast<int32>(Backend), bCooperative));
            const FString Filename = FString::Printf(TEXT("static-failure-%s%s.wasm"), Case.Name, bCooperative ? TEXT("-cooperative") : TEXT(""));
            const FString ModuleId = FString::Printf(TEXT("csharp:Scripts/StaticFailure-%s.cs"), Case.Name);
            TArray<uint8> Wasm;
            if (!TestTrue(TEXT("Read source failure fixture"), FFileHelper::LoadFileToArray(Wasm, *(Directory / Filename)))) return false;
            FAvidScriptVmBackendSelection Selection;
            Selection.BackendKind = Backend;
            Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
            FAvidScriptWasmRuntimeInstance Runtime(Selection);
            FAvidScriptWasmSmokeResult Result;
            for (int32 Domain = 0; Domain < 2; ++Domain)
            {
                if (!TestTrue(TEXT("Load fresh source failure domain"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, Result)))
                { AddError(Result.ErrorMessage); return false; }
                TestEqual(TEXT("Failure source backend"), Runtime.GetActiveBackendInfo().Kind, Backend);
                TestEqual(TEXT("Failure source mode"), Runtime.GetActiveBackendInfo().ExecutionMode, Selection.ExecutionMode);
                if (!TestTrue(TEXT("Begin source failure domain"), Runtime.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
                FAvidScriptVmPreparedExportCall Prepared;
                FString Error;
                if (!TestTrue(TEXT("Prepare source failure call"), Runtime.PrepareNamedExportCall(TEXT("run"), Prepared, Error)))
                { AddError(Error); return false; }
                for (uint32 Iteration = 0; Iteration < 16; ++Iteration)
                {
                    FAvidScriptVmCallFrame Frame;
                    Frame.CellCount = 0;
                    FAvidScriptVmCallResult Value;
                    FAvidScriptVmError VmError;
                    if (!TestTrue(TEXT("Run source failure case"), Prepared.Call(Frame, VmError, &Value)))
                    { AddError(VmError.Details); return false; }
                    TestEqual(TEXT("Source failure matches C# result and cleanup ordering"), Value.Cells[0], Case.First + Case.Increment * Iteration);
                    FHeap* Heap = Runtime.GetManagedHeapForTesting();
                    if (!TestNotNull(TEXT("Failure domain owns a heap"), Heap)) return false;
                    TestTrue(TEXT("Collect between failure calls"), Heap->Collect() == EHeapError::Ok);
                    TestEqual(TEXT("Cached error and source static roots"), Heap->GetStats().StaticRoots, Case.Roots);
                    TestEqual(TEXT("Only domain roots survive failure calls"), Heap->GetStats().LiveRoots, Case.Roots);
                    TestEqual(TEXT("Error propagation releases frames"), Heap->GetStats().ActiveFrames, 0u);
                    TestEqual(TEXT("Retain wrapper, inner errors and published aliases only"), Heap->GetStats().LiveObjects, Case.Objects);
                }
                Runtime.Unload();
                Runtime.Unload();
                TestNull(TEXT("Unload releases cached source failures"), Runtime.GetManagedHeapForTesting());
            }
        }
    }
    return true;
}
#endif
