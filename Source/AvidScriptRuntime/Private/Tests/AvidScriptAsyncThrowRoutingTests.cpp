#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "AvidScriptOriginalAsyncMemberOracle.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAsyncThrowRoutingTest,
    "AvidScript.Runtime.Continuation.CompiledAsyncThrowRouting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAsyncSynchronousExceptionsTest,
    "AvidScript.Runtime.Continuation.CompiledAsyncSynchronousExceptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAsyncMemberAssignmentsTest,
    "AvidScript.Runtime.Continuation.CompiledAsyncMemberAssignments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptStaticAsyncTest,
    "AvidScript.Runtime.Continuation.CompiledStaticAsync",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptOriginalAsyncMemberTest,
    "AvidScript.Runtime.Continuation.CompiledOriginalAsyncMember",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAwaitReadinessEvaluationTest,
    "AvidScript.Runtime.Continuation.CompiledAwaitReadinessEvaluation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace AvidScript::Tests::CompiledAsyncExceptions
{
static bool Run(FAutomationTestBase& Test, const TCHAR* FixtureVariable, int32 ExpectedScenarios, const TCHAR* LogPrefix,
    bool CollectWhileSuspended = false, bool HasStaticStorage = false, int32 ExpectedObservations = 0,
    int32 ExpectedResumeObservations = 0, bool ObserveOriginalTasks = false)
{
    if (!GEngine) return false;
    const FString Directory = FPlatformMisc::GetEnvironmentVariable(FixtureVariable);
    FString Manifest;
    TArray<TSharedPtr<FJsonValue>> Scenarios;
    if (!Test.TestTrue(TEXT("Same-source .NET fixture manifest is present"), !Directory.IsEmpty()
        && FFileHelper::LoadFileToString(Manifest, *FPaths::Combine(Directory, TEXT("cases.json")))
        && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest), Scenarios)
        && Scenarios.Num() == ExpectedScenarios)) return false;

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AsyncThrowRoutingWorld"));
    if (!Test.TestNotNull(TEXT("Async throw world created"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    int32 Cases = 0;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    {
        TSet<FString> Names;
        for (const auto& ScenarioValue : Scenarios)
        {
            const TSharedPtr<FJsonObject>* Scenario = nullptr;
            FString Name, ModuleId;
            int32 Expected = 0, ExpectedTrace = 0, Offset = -1, TraceOffset = -1;
            bool Cancel = false;
            if (!Test.TestTrue(TEXT("Fixture metadata has result, trace and cancellation expectations"),
                ScenarioValue && ScenarioValue->TryGetObject(Scenario) && Scenario && Scenario->IsValid()
                && (*Scenario)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty()
                && FPaths::GetCleanFilename(Name) == Name && !Names.Contains(Name)
                && (*Scenario)->TryGetStringField(TEXT("moduleId"), ModuleId)
                && (*Scenario)->TryGetBoolField(TEXT("cancel"), Cancel)
                && (*Scenario)->TryGetNumberField(TEXT("expected"), Expected)
                && (*Scenario)->TryGetNumberField(TEXT("trace"), ExpectedTrace)
                && (*Scenario)->TryGetNumberField(TEXT("resultOffset"), Offset) && Offset >= 0 && Offset < 65536
                && (*Scenario)->TryGetNumberField(TEXT("traceOffset"), TraceOffset) && TraceOffset >= 0 && TraceOffset < 65536)) return false;
            Names.Add(Name);
            OriginalAsyncMember::FOracle SourceOracle;
            if (ObserveOriginalTasks && !SourceOracle.Read(Test, *Scenario, Name)) return false;
            int32 StaticSlots = 0;
            if (HasStaticStorage && !Test.TestTrue(TEXT("Static fixture declares bounded domain roots"),
                (*Scenario)->TryGetNumberField(TEXT("staticSlots"), StaticSlots) && StaticSlots > 0 && StaticSlots <= 4096)) return false;
            struct FObservation { FString Name; int32 Offset = -1; int32 Expected = 0; };
            TArray<FObservation> Observations, ResumeObservations;
            auto ReadObservations = [&](const TCHAR* Field, int32 Count, TArray<FObservation>& Output) -> bool {
                if (Count == 0) return true;
                const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
                if (!Test.TestTrue(TEXT("Fixture has all observation fields"),
                    (*Scenario)->TryGetArrayField(Field, Entries) && Entries && Entries->Num() == Count)) return false;
                TSet<FString> ObservationNames;
                for (const auto& Entry : *Entries)
                {
                    const TSharedPtr<FJsonObject>* Object = nullptr;
                    FObservation Observation;
                    if (!Test.TestTrue(TEXT("Observation identity, address and expected value"), Entry && Entry->TryGetObject(Object)
                        && Object && Object->IsValid() && (*Object)->TryGetStringField(TEXT("name"), Observation.Name)
                        && !Observation.Name.IsEmpty() && !ObservationNames.Contains(Observation.Name)
                        && (*Object)->TryGetNumberField(TEXT("offset"), Observation.Offset) && Observation.Offset >= 0 && Observation.Offset < 65536
                        && (*Object)->TryGetNumberField(TEXT("expected"), Observation.Expected))) return false;
                    ObservationNames.Add(Observation.Name);
                    Output.Add(Observation);
                }
                return true;
            };
            if (!ReadObservations(TEXT("observations"), ExpectedObservations, Observations)
                || !ReadObservations(TEXT("firstResumeObservations"), ExpectedResumeObservations, ResumeObservations)) return false;
            TArray<uint8> Bytes;
            if (!Test.TestTrue(*Name, FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Directory, Name + TEXT(".wasm"))))) return false;
            // Normal completion, teardown while initially suspended, teardown
            // after the first resume. All three must retire every Task owner.
            for (int32 Mode = 0; Mode < 3; ++Mode)
            {
                const FString Label = FString::Printf(TEXT("backend=%d scenario=%s mode=%d"), static_cast<int32>(Backend), *Name, Mode);
                FAvidScriptVmBackendSelection Selection;
                Selection.BackendKind = Backend;
                Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
                    ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
                FAvidScriptWasmRuntimeInstance Runtime(Selection);
                FAvidScriptWasmSmokeResult Result;
                if (!Test.TestTrue(*Label, Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), ModuleId, Result)))
                { Test.AddError(Result.ErrorMessage); return false; }
                if (!Test.TestTrue(*Label, Runtime.ValidateRequiredExports({TEXT("avid_on_continuation_v2")}, Result)))
                { Test.AddError(Result.ErrorMessage); return false; }
                const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
                auto& Endpoint = Owner->ResetActive(World);
                CompiledCancellation::FTaskObserver TaskObserver(Endpoint);
                auto Oracle = SourceOracle;
                FAvidScriptWasmHostContext Context;
                Context.Tasks = ObserveOriginalTasks ? static_cast<IAvidScriptTaskHost*>(&TaskObserver) : &Endpoint;
                Context.Continuations = &Endpoint;
                Context.World = World;
                Runtime.SetHostContext(Context);
                ON_SCOPE_EXIT { Owner->Teardown(); };
                if (!Test.TestTrue(*Label, Runtime.BeginPlay(Result))) { Test.AddError(Result.ErrorMessage); return false; }
                if (ObserveOriginalTasks) TaskObserver.CaptureEntryStates();
                auto Collect = [&]() -> bool {
                    if (!CollectWhileSuspended) return true;
                    auto* Heap = Runtime.GetManagedHeapForTesting();
                    return Test.TestNotNull(*(Label + TEXT(" managed heap")), Heap)
                        && Test.TestEqual(*(Label + TEXT(" suspended collection")), Heap->Collect(), AvidScript::Managed::EHeapError::Ok);
                };
                if (!Collect()) return false;
                if (ObserveOriginalTasks && Mode == 0 && !Oracle.CheckSuspension(Test, Label, TaskObserver, Runtime)) return false;
                auto Read = [&](int32 Address) -> int32 {
                    uint8 Data[4] = {};
                    FString Error;
                    if (!Runtime.ReadStateBytes(Address, MakeArrayView(Data), Error)) { Test.AddError(Error); return MIN_int32; }
                    int32 Value = 0;
                    FMemory::Memcpy(&Value, Data, sizeof(Value));
                    return Value;
                };
                bool CheckedFirstResume = false;
                auto CheckFirstResume = [&]() {
                    for (const auto& Observation : ResumeObservations)
                        Test.TestEqual(*(Label + TEXT(" first resume ") + Observation.Name),
                            Read(Observation.Offset), Observation.Expected);
                    CheckedFirstResume = true;
                };
                bool Stopped = Mode == 1;
                int32 StoppedResult = Read(Offset), StoppedTrace = Read(TraceOffset), Resumes = 0;
                TArray<int32> StoppedObservations;
                auto SnapshotObservations = [&]() {
                    StoppedObservations.Reset();
                    for (const auto& Observation : Observations) StoppedObservations.Add(Read(Observation.Offset));
                };
                SnapshotObservations();
                if (Stopped) Owner->Teardown();
                else if (Cancel)
                {
                    int64 Token = 0, Producer = 0;
                    if (!Test.TestTrue(*Label, Owner->GetPendingActiveTimerForTesting(Token, Producer) && Endpoint.Cancel(Token))) return false;
                }
                for (int32 Round = 0; Round < 64; ++Round)
                {
                    World->Tick(LEVELTICK_All, 0.02f);
                    ++GFrameCounter;
                    TArray<FAvidScriptContinuationCompletion> Ready;
                    Owner->DrainReady(Ready);
                    if (Stopped) { Test.TestEqual(*Label, Ready.Num(), 0); continue; }
                    for (const auto& Completion : Ready)
                    {
                        if (!Collect()) return false;
                        if (!Test.TestTrue(*Label, Runtime.DispatchContinuation(Completion, Result)))
                        { Test.AddError(Result.ErrorMessage); return false; }
                        Test.TestTrue(*Label, Owner->FinalizeDispatched(Completion.Token, true));
                        if (!Collect()) return false;
                        ++Resumes;
                        if (ObserveOriginalTasks && Mode == 0 && !Oracle.CheckSuspension(Test, Label, TaskObserver, Runtime)) return false;
                        if (!CheckedFirstResume) CheckFirstResume();
                    }
                    if (Mode == 2 && Resumes > 0)
                    {
                        StoppedResult = Read(Offset);
                        StoppedTrace = Read(TraceOffset);
                        SnapshotObservations();
                        Owner->Teardown();
                        Stopped = true;
                    }
                }
                // A pre-cancelled entry can finish without scheduling anything.
                // It must already have the reference side effects in that case.
                if (!Stopped && !CheckedFirstResume) CheckFirstResume();
                if (ObserveOriginalTasks && Mode == 0 && !Oracle.CheckFinal(Test, Label, TaskObserver)) return false;
                Test.TestEqual(*Label, Read(Offset), Stopped ? StoppedResult : Expected);
                Test.TestEqual(*(Label + TEXT(" cleanup order")), Read(TraceOffset), Stopped ? StoppedTrace : ExpectedTrace);
                for (int32 Index = 0; Index < Observations.Num(); ++Index)
                    Test.TestEqual(*(Label + TEXT(" ") + Observations[Index].Name), Read(Observations[Index].Offset),
                        Stopped ? StoppedObservations[Index] : Observations[Index].Expected);
                Test.TestEqual(*(Label + TEXT(" tasks")), Owner->GetTaskResultsForTesting().GetCount(), 0);
                Test.TestEqual(*(Label + TEXT(" waiters")), Owner->GetTaskResultsForTesting().GetWaiterCount(), 0);
                Test.TestEqual(*(Label + TEXT(" continuations")), Owner->GetActiveCount(), 0);
                Test.TestEqual(*(Label + TEXT(" state frames")), Owner->GetStateFrameByteCountForTesting(), 0);
                if (auto* Heap = Runtime.GetManagedHeapForTesting())
                {
                    Test.TestEqual(*(Label + TEXT(" domain roots")), Heap->GetStats().StaticRoots, static_cast<uint32>(StaticSlots));
                    Test.TestEqual(*(Label + TEXT(" roots")), Heap->GetStats().LiveRoots, static_cast<uint32>(StaticSlots));
                    Test.TestEqual(*(Label + TEXT(" heap frames")), Heap->GetStats().ActiveFrames, static_cast<uint32>(0));
                    Test.TestEqual(*Label, Heap->Collect(), AvidScript::Managed::EHeapError::Ok);
                    if (!HasStaticStorage)
                        Test.TestEqual(*(Label + TEXT(" objects")), Heap->GetStats().LiveObjects, static_cast<uint32>(0));
                }
                ++Cases;
                Test.AddInfo(FString::Printf(TEXT("%s %s result=%d trace=%d resumes=%d"), LogPrefix, *Label, Read(Offset), Read(TraceOffset), Resumes));
                Runtime.Unload();
                Runtime.Unload();
                Test.TestNull(*(Label + TEXT(" unload releases domain storage")), Runtime.GetManagedHeapForTesting());
            }
        }
    }
    Test.TestEqual(TEXT("Async exception scenario count"), Cases, ExpectedScenarios * 6);
    return true;
}
}

bool FAvidScriptAsyncThrowRoutingTest::RunTest(const FString& Parameters)
{
    return AvidScript::Tests::CompiledAsyncExceptions::Run(*this,
        TEXT("AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR"), 27, TEXT("async-throw"));
}

bool FAvidScriptAsyncSynchronousExceptionsTest::RunTest(const FString& Parameters)
{
    return AvidScript::Tests::CompiledAsyncExceptions::Run(*this,
        TEXT("AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR"), 31, TEXT("async-synchronous"));
}

bool FAvidScriptAsyncMemberAssignmentsTest::RunTest(const FString& Parameters)
{
    return AvidScript::Tests::CompiledAsyncExceptions::Run(*this,
        TEXT("AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR"), 47, TEXT("async-member"), true);
}

bool FAvidScriptStaticAsyncTest::RunTest(const FString& Parameters)
{
    return AvidScript::Tests::CompiledAsyncExceptions::Run(*this,
        TEXT("AVIDSCRIPT_STATIC_ASYNC_FIXTURE_DIR"), 37, TEXT("static-async"), true, true);
}

bool FAvidScriptOriginalAsyncMemberTest::RunTest(const FString& Parameters)
{
    return AvidScript::Tests::CompiledAsyncExceptions::Run(*this,
        TEXT("AVIDSCRIPT_ORIGINAL_ASYNC_MEMBER_DIR"), 29, TEXT("original-async-member"), true, true, 18, 0, true);
}

bool FAvidScriptAwaitReadinessEvaluationTest::RunTest(const FString& Parameters)
{
    return AvidScript::Tests::CompiledAsyncExceptions::Run(*this,
        TEXT("AVIDSCRIPT_AWAIT_READINESS_DIR"), 18, TEXT("await-readiness"), true, false, 9, 4);
}

#endif
