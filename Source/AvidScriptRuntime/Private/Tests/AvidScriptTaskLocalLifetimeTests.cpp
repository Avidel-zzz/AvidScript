#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskLocalLifetimeTest,
    "AvidScript.Runtime.Continuation.CompiledTaskLocalLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskLocalLifetimeTest::RunTest(const FString& Parameters)
{
    if (!GEngine) return false;
    const FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_TASK_LIFETIME_FIXTURE_DIR"));
    if (!TestFalse(TEXT("Task lifetime fixture directory is supplied"), Directory.IsEmpty())) return false;
    TArray<FString> Scenarios;
    for (const TCHAR* Name : {TEXT("replace"), TEXT("branch"), TEXT("for"), TEXT("while"), TEXT("do"), TEXT("nested"), TEXT("unused")})
        for (const TCHAR* Completion : {TEXT("ready"), TEXT("deferred")})
            Scenarios.Add(FString::Printf(TEXT("%s-%s"), Name, Completion));
    for (const TCHAR* Completion : {TEXT("ready"), TEXT("deferred")})
        for (const TCHAR* Suffix : {TEXT(""), TEXT("-cancel")})
        {
            Scenarios.Add(FString::Printf(TEXT("fault-%s%s"), Completion, Suffix));
            Scenarios.Add(FString::Printf(TEXT("timer-fault-%s%s"), Completion, Suffix));
        }
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("TaskLocalLifetimeWorld"));
    if (!TestNotNull(TEXT("Task lifetime world created"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    int32 Cases = 0;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (const FString& Scenario : Scenarios)
    for (int32 Mode = 0; Mode < (Scenario == TEXT("timer-fault-ready-cancel") ? 4 : 3); ++Mode)
    {
        const FString Label = FString::Printf(TEXT("backend=%d scenario=%s mode=%d"), static_cast<int32>(Backend), *Scenario, Mode);
        const FString Stem = FPaths::Combine(Directory, TEXT("task-lifetime-") + Scenario);
        TArray<uint8> Bytes;
        FString ModuleId, OffsetText, ExpectedText;
        int32 Offset = -1, Expected = 0;
        if (!TestTrue(*Label, FFileHelper::LoadFileToArray(Bytes, *(Stem + TEXT(".wasm")))
            && FFileHelper::LoadFileToString(ModuleId, *(Stem + TEXT(".module-id")))
            && FFileHelper::LoadFileToString(OffsetText, *(Stem + TEXT(".result-offset")))
            && FFileHelper::LoadFileToString(ExpectedText, *(Stem + TEXT(".expected")))
            && LexTryParseString(Offset, *OffsetText) && Offset >= 0 && Offset < 65536
            && LexTryParseString(Expected, *ExpectedText))) return false;
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
        int32 StoppedResult = Read(Offset);
        int32 Resumes = 0;
        if (Stopped) Owner->Teardown();
        if (Mode == 3)
        {
            int64 Token = 0, Producer = 0;
            if (!TestTrue(*Label, Owner->GetPendingActiveTimerForTesting(Token, Producer) && Endpoint.Cancel(Token))) return false;
            Expected = 9;
        }
        for (int32 Round = 0; Round < 64; ++Round)
        {
            World->Tick(LEVELTICK_All, 0.02f);
            ++GFrameCounter;
            TArray<FAvidScriptContinuationCompletion> Ready;
            Owner->DrainReady(Ready);
            if (Stopped)
            {
                TestEqual(*Label, Ready.Num(), 0);
                continue;
            }
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
                Owner->Teardown();
                Stopped = true;
            }
        }
        TestEqual(*Label, Read(Offset), Stopped ? StoppedResult : Expected);
        TestEqual(*Label, Owner->GetTaskResultsForTesting().GetCount(), 0);
        TestEqual(*Label, Owner->GetTaskResultsForTesting().GetWaiterCount(), 0);
        TestEqual(*Label, Owner->GetActiveCount(), 0);
        TestEqual(*Label, Owner->GetStateFrameByteCountForTesting(), 0);
        if (!Stopped && (Scenario.StartsWith(TEXT("fault-")) || Scenario.StartsWith(TEXT("timer-"))))
        {
            FString CleanupText;
            int32 CleanupOffset = -1;
            if (!TestTrue(*Label, FFileHelper::LoadFileToString(CleanupText, *(Stem + TEXT(".cleanup-offset")))
                && LexTryParseString(CleanupOffset, *CleanupText) && CleanupOffset >= 0 && CleanupOffset < 65536)) return false;
            TestEqual(*Label, Read(CleanupOffset), 1);
        }
        Owner->Teardown();
        if (auto* Heap = Runtime.GetManagedHeapForTesting())
        {
            TestEqual(*Label, Heap->GetStats().LiveRoots, static_cast<uint32>(0));
            if (Runtime.GetLanguageErrorCatalog())
                TestEqual(*Label, Heap->Collect(), AvidScript::Managed::EHeapError::Ok);
            TestEqual(*Label, Heap->GetStats().LiveObjects, static_cast<uint32>(0));
        }
        ++Cases;
        AddInfo(FString::Printf(TEXT("task-lifetime %s result=%d resumes=%d"), *Label, Read(Offset), Resumes));
    }
    TestEqual(TEXT("Task lifetime scenario count"), Cases, 134);
    return true;
}

#endif
