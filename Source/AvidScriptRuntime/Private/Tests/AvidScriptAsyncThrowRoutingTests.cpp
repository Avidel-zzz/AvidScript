#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
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

bool FAvidScriptAsyncThrowRoutingTest::RunTest(const FString& Parameters)
{
    if (!GEngine) return false;
    const FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR"));
    FString Manifest;
    TArray<TSharedPtr<FJsonValue>> Scenarios;
    if (!TestTrue(TEXT("Same-source .NET fixture manifest is present"), !Directory.IsEmpty()
        && FFileHelper::LoadFileToString(Manifest, *FPaths::Combine(Directory, TEXT("cases.json")))
        && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest), Scenarios)
        && Scenarios.Num() == 27)) return false;

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AsyncThrowRoutingWorld"));
    if (!TestNotNull(TEXT("Async throw world created"), World)) return false;
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
            if (!TestTrue(TEXT("Fixture metadata has result, trace and cancellation expectations"),
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
            TArray<uint8> Bytes;
            if (!TestTrue(*Name, FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Directory, Name + TEXT(".wasm"))))) return false;
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
                if (!TestTrue(*Label, Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), ModuleId, Result)))
                { AddError(Result.ErrorMessage); return false; }
                if (!TestTrue(*Label, Runtime.ValidateRequiredExports({TEXT("avid_on_continuation_v2")}, Result)))
                { AddError(Result.ErrorMessage); return false; }
                const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
                auto& Endpoint = Owner->ResetActive(World);
                FAvidScriptWasmHostContext Context;
                Context.Tasks = &Endpoint;
                Context.Continuations = &Endpoint;
                Context.World = World;
                Runtime.SetHostContext(Context);
                ON_SCOPE_EXIT { Owner->Teardown(); };
                if (!TestTrue(*Label, Runtime.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
                auto Read = [&](int32 Address) -> int32 {
                    uint8 Data[4] = {};
                    FString Error;
                    if (!Runtime.ReadStateBytes(Address, MakeArrayView(Data), Error)) { AddError(Error); return MIN_int32; }
                    int32 Value = 0;
                    FMemory::Memcpy(&Value, Data, sizeof(Value));
                    return Value;
                };
                bool Stopped = Mode == 1;
                int32 StoppedResult = Read(Offset), StoppedTrace = Read(TraceOffset), Resumes = 0;
                if (Stopped) Owner->Teardown();
                else if (Cancel)
                {
                    int64 Token = 0, Producer = 0;
                    if (!TestTrue(*Label, Owner->GetPendingActiveTimerForTesting(Token, Producer) && Endpoint.Cancel(Token))) return false;
                }
                for (int32 Round = 0; Round < 64; ++Round)
                {
                    World->Tick(LEVELTICK_All, 0.02f);
                    ++GFrameCounter;
                    TArray<FAvidScriptContinuationCompletion> Ready;
                    Owner->DrainReady(Ready);
                    if (Stopped) { TestEqual(*Label, Ready.Num(), 0); continue; }
                    for (const auto& Completion : Ready)
                    {
                        if (!TestTrue(*Label, Runtime.DispatchContinuation(Completion, Result)))
                        { AddError(Result.ErrorMessage); return false; }
                        TestTrue(*Label, Owner->FinalizeDispatched(Completion.Token, true));
                        ++Resumes;
                    }
                    if (Mode == 2 && Resumes > 0)
                    {
                        StoppedResult = Read(Offset);
                        StoppedTrace = Read(TraceOffset);
                        Owner->Teardown();
                        Stopped = true;
                    }
                }
                TestEqual(*Label, Read(Offset), Stopped ? StoppedResult : Expected);
                TestEqual(*(Label + TEXT(" cleanup order")), Read(TraceOffset), Stopped ? StoppedTrace : ExpectedTrace);
                TestEqual(*(Label + TEXT(" tasks")), Owner->GetTaskResultsForTesting().GetCount(), 0);
                TestEqual(*(Label + TEXT(" waiters")), Owner->GetTaskResultsForTesting().GetWaiterCount(), 0);
                TestEqual(*(Label + TEXT(" continuations")), Owner->GetActiveCount(), 0);
                TestEqual(*(Label + TEXT(" state frames")), Owner->GetStateFrameByteCountForTesting(), 0);
                if (auto* Heap = Runtime.GetManagedHeapForTesting())
                {
                    TestEqual(*(Label + TEXT(" roots")), Heap->GetStats().LiveRoots, static_cast<uint32>(0));
                    TestEqual(*Label, Heap->Collect(), AvidScript::Managed::EHeapError::Ok);
                    TestEqual(*(Label + TEXT(" objects")), Heap->GetStats().LiveObjects, static_cast<uint32>(0));
                }
                ++Cases;
                AddInfo(FString::Printf(TEXT("async-throw %s result=%d trace=%d resumes=%d"), *Label, Read(Offset), Read(TraceOffset), Resumes));
            }
        }
    }
    TestEqual(TEXT("Async throw scenario count"), Cases, 162);
    return true;
}

#endif
