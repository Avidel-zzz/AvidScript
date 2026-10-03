#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "AvidScriptObjectRegistry.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAsyncVoidErrorOwnerTest,
    "AvidScript.Runtime.Continuation.CompiledAsyncVoidErrorOwners",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptAsyncVoidErrorOwnerTest::RunTest(const FString& Parameters)
{
    if (!GEngine) return false;
    const FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_ASYNC_VOID_OWNER_FIXTURE_DIR"));
    FString Manifest;
    TArray<TSharedPtr<FJsonValue>> Scenarios;
    if (!TestTrue(TEXT("Same-source async void reference manifest is present"), !Directory.IsEmpty()
        && FFileHelper::LoadFileToString(Manifest, *FPaths::Combine(Directory, TEXT("cases.json")))
        && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest), Scenarios)
        && Scenarios.Num() == 24)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AsyncVoidOwnerWorld"));
    if (!TestNotNull(TEXT("Async void test World exists"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    AActor* Actor = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("Contextual callback owner exists"), Actor)) return false;
    int32 Cases = 0;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    {
        TSet<FString> Names;
        for (const auto& Value : Scenarios)
        {
            const TSharedPtr<FJsonObject>* Scenario = nullptr;
            FString Name, ModuleId, ErrorType;
            bool Cancel = false;
            int32 TraceOffset = -1, ExpectedTrace = 0;
            int32 StaticRoots = 0;
            bool StaticSuccessDeferred = false, StaticCacheFault = false, CacheDeferred = false;
            if (!TestTrue(TEXT("Fixture contains bounded unique identity and reference expectations"),
                Value && Value->TryGetObject(Scenario) && Scenario && Scenario->IsValid()
                && (*Scenario)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty()
                && FPaths::GetCleanFilename(Name) == Name && !Names.Contains(Name)
                && (*Scenario)->TryGetStringField(TEXT("moduleId"), ModuleId)
                && (*Scenario)->TryGetStringField(TEXT("errorType"), ErrorType)
                && (*Scenario)->TryGetBoolField(TEXT("cancel"), Cancel)
                && (*Scenario)->TryGetNumberField(TEXT("trace"), ExpectedTrace)
                && (*Scenario)->TryGetNumberField(TEXT("traceOffset"), TraceOffset)
                && TraceOffset >= 0 && TraceOffset <= 65532)) return false;
            Names.Add(Name);
            // These fields exist only on the composition fixtures. IR36 retains
            // its original zero-resource assertions below.
            (*Scenario)->TryGetNumberField(TEXT("staticRoots"), StaticRoots);
            (*Scenario)->TryGetBoolField(TEXT("staticSuccessDeferred"), StaticSuccessDeferred);
            (*Scenario)->TryGetBoolField(TEXT("staticCacheFault"), StaticCacheFault);
            (*Scenario)->TryGetBoolField(TEXT("cacheDeferred"), CacheDeferred);
            if (!TestTrue(TEXT("Static fixture expectations are bounded"), StaticRoots >= 0 && StaticRoots <= 256
                && (StaticRoots > 0 || (!StaticSuccessDeferred && !StaticCacheFault && !CacheDeferred)))) return false;
            TArray<uint8> Bytes;
            if (!TestTrue(*Name, FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Directory, Name + TEXT(".wasm"))))) return false;
            // Complete; retire during initial suspension; retire after first
            // resume; complete through the contextual production entry path.
            for (int32 Mode = 0; Mode < 4; ++Mode)
            {
                const FString Label = FString::Printf(TEXT("backend=%d scenario=%s mode=%d"), static_cast<int32>(Backend), *Name, Mode);
                FAvidScriptVmBackendSelection Selection;
                Selection.BackendKind = Backend;
                Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
                    ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
                FAvidScriptWasmRuntimeInstance Runtime(Selection);
                FAvidScriptWasmSmokeResult Result;
                if (!TestTrue(*Label, Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), ModuleId, Result)))
                { AddError(Result.ErrorMessage); return false; }
                if (!TestTrue(*(Label + TEXT(" continuation export")), Runtime.ValidateRequiredExports({TEXT("avid_on_continuation_v2")}, Result)))
                { AddError(Result.ErrorMessage); return false; }
                const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
                auto& Endpoint = Owner->ResetActive(World);
                ON_SCOPE_EXIT { Owner->Teardown(); };
                FAvidScriptObjectRegistry Registry;
                FAvidScriptObjectHandleResult HandleResult;
                FAvidScriptWasmHostContext Context;
                Context.Tasks = &Endpoint;
                Context.Continuations = &Endpoint;
                Context.World = World;
                Context.ObjectRegistry = &Registry;
                Context.OwnerHandle = Registry.RegisterObject(Actor, HandleResult, false);
                Runtime.SetHostContext(Context);
                FAvidScriptContextualExportCall Continuation;
                FString PrepareError;
                if (Mode == 3 && !TestTrue(*(Label + TEXT(" owner-bound instance state")),
                    Runtime.CreateInstanceExecutionState(Context, Context.InstanceExecutionState, PrepareError)))
                { AddError(PrepareError); return false; }
                if (Mode == 3 && !TestTrue(*(Label + TEXT(" contextual continuation")),
                    Runtime.PrepareContextualContinuationCall(Continuation, PrepareError)))
                { AddError(PrepareError); return false; }
                auto* Heap = Runtime.GetManagedHeapForTesting();
                if (!TestNotNull(*(Label + TEXT(" managed heap")), Heap)) return false;
                auto Collect = [&]() {
                    const auto Collection = Heap->Collect();
                    const auto Stats = Heap->GetStats();
                    return TestTrue(*(Label + TEXT(" collection or empty unconfigured heap at callback boundary")),
                        Collection == AvidScript::Managed::EHeapError::Ok
                            || (Collection == AvidScript::Managed::EHeapError::NotConfigured && Stats.LiveObjects == 0
                                && Stats.LiveRoots == 0 && Stats.ActiveFrames == 0 && Stats.NativeDataBytes == 0));
                };
                auto ReadTrace = [&]() -> int32 {
                    uint8 Data[4] = {};
                    FString Error;
                    if (!Runtime.ReadStateBytes(TraceOffset, MakeArrayView(Data), Error)) { AddError(Error); return MIN_int32; }
                    int32 Trace = 0;
                    FMemory::Memcpy(&Trace, Data, sizeof(Trace));
                    return Trace;
                };
                auto CheckFault = [&]() {
                    TestFalse(*(Label + TEXT(" reference expects an error")), ErrorType.IsEmpty());
                    TestEqual(*(Label + TEXT(" session fault category")), Result.ErrorCategory, FString(TEXT("language_error_uncaught")));
                    TestEqual(*(Label + TEXT(" checked report import")), Result.ImportName, FString(TEXT("avid_language_error_report_v1")));
                    TestTrue(*(Label + TEXT(" original error type and source")), Result.ErrorMessage.Contains(ErrorType)
                        && Result.ErrorMessage.Contains(TEXT("Scripts/AsyncVoidOwner_")));
                    TestEqual(*(Label + TEXT(" faulted lifecycle")), Context.InstanceExecutionState
                        ? Context.InstanceExecutionState->GetLifecycleState() : Runtime.GetLifecycleState(), EAvidScriptLifecycleState::Faulted);
                };
                const bool Began = Mode == 3 ? Runtime.BeginPlayInContext(Context, Result) : Runtime.BeginPlay(Result);
                bool Faulted = !Began;
                if (Faulted) CheckFault();
                if (!Collect()) return false;
                bool Stopped = Mode == 1 || Faulted;
                int32 FrozenTrace = ReadTrace(), Resumes = 0;
                if (Mode == 1) Owner->Teardown();
                else if (Cancel && !Faulted)
                {
                    int64 Token = 0, Producer = 0;
                    if (!TestTrue(*(Label + TEXT(" cancellation enters active Timer")),
                        Owner->GetPendingActiveTimerForTesting(Token, Producer, true) && Endpoint.Cancel(Token))) return false;
                    TestEqual(*(Label + TEXT(" async void Timer has no public producer Task")), Producer, int64(0));
                }
                for (int32 Round = 0; Round < 64; ++Round)
                {
                    World->Tick(LEVELTICK_All, 0.02f);
                    ++GFrameCounter;
                    TArray<FAvidScriptContinuationCompletion> Ready;
                    Owner->DrainReady(Ready);
                    if (Stopped) { TestEqual(*(Label + TEXT(" no entry after fault or teardown")), Ready.Num(), 0); continue; }
                    for (const auto& Completion : Ready)
                    {
                        if (!Collect()) return false;
                        const bool Called = Mode == 3
                            ? Runtime.DispatchContinuationInContext(Continuation, Context, Completion, Result)
                            : Runtime.DispatchContinuation(Completion, Result);
                        TestTrue(*(Label + TEXT(" continuation finalizes")), Owner->FinalizeDispatched(Completion.Token, Called));
                        ++Resumes;
                        if (!Called) { Faulted = true; CheckFault(); }
                        if (!Collect()) return false;
                    }
                    if (Faulted || (Mode == 2 && Resumes > 0))
                    {
                        FrozenTrace = ReadTrace();
                        Stopped = true;
                        if (Mode == 2) Owner->Teardown();
                    }
                }
                const bool Complete = Mode == 0 || Mode == 3;
                if (Complete) TestEqual(*(Label + TEXT(" matches .NET unhandled outcome")), Faulted, !ErrorType.IsEmpty());
                TestEqual(*(Label + TEXT(" finally cleanup order")), ReadTrace(), Complete ? ExpectedTrace : Stopped ? FrozenTrace : ExpectedTrace);
                // On completed/faulted paths these checks run BEFORE Session
                // teardown, so teardown cannot hide a missing Guest owner exit.
                TestEqual(*(Label + TEXT(" Task owners before teardown")), Owner->GetTaskResultsForTesting().GetCount(), 0);
                TestEqual(*(Label + TEXT(" waiters")), Owner->GetTaskResultsForTesting().GetWaiterCount(), 0);
                TestEqual(*(Label + TEXT(" continuations")), Owner->GetActiveCount(), 0);
                TestEqual(*(Label + TEXT(" state frames")), Owner->GetStateFrameByteCountForTesting(), 0);
                TestEqual(*(Label + TEXT(" cancellation sources before teardown")), Owner->GetCancellationSourceCountForTesting(), 0);
                TestEqual(*(Label + TEXT(" static domain roots")), Heap->GetStats().StaticRoots, static_cast<uint32>(StaticRoots));
                TestEqual(*(Label + TEXT(" only domain roots before teardown")), Heap->GetStats().LiveRoots, static_cast<uint32>(StaticRoots));
                TestEqual(*(Label + TEXT(" managed frames")), Heap->GetStats().ActiveFrames, 0u);
                if (!Collect()) return false;
                const bool CacheTouched = StaticCacheFault && (!CacheDeferred || Resumes > 0);
                const bool SuccessTouched = StaticSuccessDeferred && Resumes > 0;
                // Script.Trace has no initializer, so its reserved domain slot
                // remains null. Cache creates one control object; Broken adds
                // its control object, wrapper and inner error until unload.
                const uint32 DomainObjects = CacheTouched ? 3u : SuccessTouched ? 1u : 0u;
                TestEqual(*(Label + TEXT(" only expected domain controls and cached errors survive")), Heap->GetStats().LiveObjects, DomainObjects);
                if (DomainObjects == 0)
                    TestEqual(*(Label + TEXT(" heap native bytes")), Heap->GetStats().NativeDataBytes, uint64(0));
                Owner->Teardown();
                Owner->Teardown();
                Runtime.Unload();
                Runtime.Unload();
                TestNull(*(Label + TEXT(" VM retirement releases domain controls and cached errors")), Runtime.GetManagedHeapForTesting());
                ++Cases;
                AddInfo(FString::Printf(TEXT("async-void-owner %s fault=%d trace=%d resumes=%d domain_roots=%d domain_objects=%u"),
                    *Label, Faulted ? 1 : 0, Complete ? ExpectedTrace : FrozenTrace, Resumes, StaticRoots, DomainObjects));
            }
        }
    }
    TestEqual(TEXT("Two backends, four lifecycle/context paths, twenty-four source cases"), Cases, 192);
    return true;
}

#endif
