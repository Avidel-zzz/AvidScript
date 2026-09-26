#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptTypedCancellationTestSupport.h"
#include "AvidScriptRuntimeSession.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskLifetimeLifecycleTest,
    "AvidScript.Runtime.Continuation.TaskLocalLifetimeLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskLifetimeLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::TypedCancellation;
    if (!GEngine) return false;
    FFixture Initial, Next;
    const TCHAR* Entry = TEXT("TaskLifetimeLifecycleEntry");
    const TCHAR* Script = TEXT("TaskLifetimeScript");
    if (!Initial.Load(*this, FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_TASK_LIFETIME_MANIFEST")),
            25, Entry, TEXT("task_lifetime_lifecycle"))
        || !Next.Load(*this, FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_TASK_LIFETIME_NEXT_MANIFEST")),
            25, Entry, TEXT("task_lifetime_lifecycle"))) return false;
    TestEqual(TEXT("Code generations retain module identity"), Initial.Manifest.ModuleId, Next.Manifest.ModuleId);
    TestTrue(TEXT("Code generations have different executable bytes"), Initial.Bytes != Next.Bytes);
    TestEqual(TEXT("Readback field count unchanged"), Initial.Offsets.Num(), Next.Offsets.Num());
    for (const auto& Slot : Initial.Offsets)
    {
        const auto* Updated = Next.Offsets.Find(Slot.Key);
        if (!TestTrue(TEXT("Both generations retain compiler memory layout"), Updated && *Updated == Slot.Value)) return false;
    }
    int32 Cases = 0;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (int32 Stage = -1; Stage < 3; ++Stage)
    for (int32 Mode = -3; Mode < 3; ++Mode)
    {
        // Stages: pending, cancellation queued, child errors rooted, outer catch
        // completed with both Task locals held across an unrelated next-tick wait.
        // Modes: stop, World teardown, Actor destruction, commit, cancelled commit,
        // and rollback after the actual candidate has created suspended work.
        const FString Label = FString::Printf(TEXT("backend=%d stage=%d mode=%d"), static_cast<int32>(Backend), Stage, Mode);
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        if (!TestNotNull(*Label, World)) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
        FAvidScriptObjectRegistry Registry;
        AActor* Actor = World->SpawnActor<AActor>();
        if (!TestNotNull(*Label, Actor)) return false;
        FAvidScriptObjectHandleResult Registered;
        const auto Handle = Registry.RegisterObject(Actor, Registered, false);
        if (!TestTrue(*Label, Handle.IsValid())) return false;
        FAvidScriptRuntimeSession Session;
        ON_SCOPE_EXIT { FAvidScriptWasmSmokeResult Stopped; Session.StopAndUnload(Stopped); };
        Session.SetBackendSelectionForTesting(Selection(Backend));
        FAvidScriptWasmHostContext Context;
        Context.World = World; Context.ObjectRegistry = &Registry; Context.OwnerHandle = Handle;
        Session.SetHostContext(Context);
        FAvidScriptWasmReloadResult Reload;
        if (!Session.LoadInitialModule(Initial.Bytes.GetData(), Initial.Bytes.Num(), Initial.Manifest, Reload))
        { AddError(Label + TEXT(": ") + Reload.ErrorMessage); return false; }
        auto* Owner = Session.GetContinuationOwnerForTesting();
        auto* Original = Session.GetLiveRuntimeForTesting();
        const auto OriginalLease = Session.GetRuntimeLeaseForTesting();
        auto Read = [&](const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field)
        { return Initial.Read(*this, Runtime, Script, Field); };
        auto ReadEntry = [&](const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field)
        { return Initial.Read(*this, Runtime, Entry, Field); };
        FAvidScriptWasmSmokeResult Call;
        if (!Session.DispatchEventLive(FMath::Max(Mode, 0), Stage >= 0 ? -1.0f : 0.0f, Call))
        { AddError(Call.ErrorMessage); return false; }
        // The Session dispatcher has a per-tick budget. Reach the source stage
        // through observed progress instead of assuming both child callbacks fit
        // into one TickLive invocation; do not advance the World timer here.
        if (Stage > 0)
            for (int32 Step = 0; Step < 8 && Read(*Original, TEXT("ChildFinally")) < 2; ++Step)
                if (!Session.TickLive(0.0f, Call)) { AddError(Call.ErrorMessage); return false; }
        if (Stage == 2)
            for (int32 Step = 0; Step < 8 && Read(*Original, TEXT("OuterFinally")) < 1; ++Step)
                if (!Session.TickLive(0.0f, Call)) { AddError(Call.ErrorMessage); return false; }
        TestEqual(*Label, ReadEntry(*Original, TEXT("BeginCount")), 1);
        TestEqual(*Label, ReadEntry(*Original, TEXT("Result")), 0);
        TestEqual(*Label, Read(*Original, TEXT("ChildFinally")), Stage > 0 ? 2 : 0);
        TestEqual(*Label, Read(*Original, TEXT("InnerCatch")), Stage > 0 ? 2 : 0);
        TestEqual(*Label, Read(*Original, TEXT("OuterCatch")), Stage == 2 ? 1 : 0);
        TestEqual(*Label, Read(*Original, TEXT("OuterFinally")), Stage == 2 ? 1 : 0);
        TestEqual(*Label, Read(*Original, TEXT("AfterWait")), 0);
        TestEqual(TEXT("Replacement and old alias own distinct child Tasks"), Owner->GetTaskResultsForTesting().GetCount(), 3);
        const int32 OldFrames = Owner->GetStateFrameByteCountForTesting();
        const int32 OldReady = Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active);
        const uint32 OldRoots = Original->GetManagedHeapForTesting()->GetStats().LiveRoots;
        TestTrue(*Label, OldFrames > 0);
        if (Stage > 0)
        {
            TestTrue(TEXT("Both cancelled Tasks retain language errors"), OldRoots >= 2);
            const uint32 OldObjects = Original->GetManagedHeapForTesting()->GetStats().LiveObjects;
            TestTrue(*Label, Original->GetManagedHeapForTesting()->Collect() == AvidScript::Managed::EHeapError::Ok);
            TestEqual(TEXT("Live Task error objects survive collection"), Original->GetManagedHeapForTesting()->GetStats().LiveObjects, OldObjects);
        }
        TArray<int32> Frozen;
        const TCHAR* Fields[] = {TEXT("ChildFinally"), TEXT("InnerCatch"), TEXT("OuterCatch"),
            TEXT("OuterFinally"), TEXT("Iterations"), TEXT("AfterWait")};
        for (const TCHAR* Field : Fields) Frozen.Add(Read(*Original, Field));
        TWeakPtr<FAvidScriptWasmRuntimeInstance> CandidateLease;
        int32 Expected = 0;
        if (Mode < 0)
        {
            if (Mode == -3) TestTrue(*Label, Session.StopAndUnload(Call));
            else if (Mode == -2) World->BeginTearingDown();
            else
            {
                TestTrue(TEXT("Actor actually destroyed"), World->DestroyActor(Actor));
                FAvidScriptObjectHandleResult Resolution;
                TestNull(TEXT("Destroyed Actor no longer resolves"), Registry.ResolveObject(Handle, Resolution, false));
            }
            for (int32 Round = 0; Round < 8; ++Round)
            {
                if (Mode != -2) { World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter; }
                TArray<FAvidScriptContinuationCompletion> Ready;
                Owner->DrainReady(Ready);
                TestEqual(TEXT("Retired work never re-enters Guest"), Ready.Num(), 0);
            }
            if (Mode != -3)
            {
                for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
                    TestEqual(*Label, Read(*Original, Fields[Index]), Frozen[Index]);
                TestEqual(*Label, ReadEntry(*Original, TEXT("Result")), 0);
                CheckHeapReleased(*this, *Original);
            }
            CheckEmpty(*this, *Owner, 0);
        }
        else
        {
            const bool Reject = Mode == 2;
            int32 Observed = 0;
            Session.SetCandidateBeginPlayCompletionObserverForTesting(
                [&](TWeakPtr<FAvidScriptWasmRuntimeInstance> Candidate, bool Began)
                {
                    ++Observed; CandidateLease = Candidate;
                    const auto Runtime = Candidate.Pin();
                    if (!TestTrue(*Label, Runtime.IsValid())) return;
                    TestTrue(TEXT("Candidate owns a different VM"), Runtime.Get() != Original);
                    TestEqual(*Label, Began, !Reject);
                    TestEqual(*Label, ReadEntry(*Runtime, TEXT("BeginCount")), 2);
                    TestEqual(*Label, ReadEntry(*Runtime, TEXT("ReloadMode")), Mode);
                    for (const TCHAR* Field : Fields) TestEqual(*Label, Read(*Runtime, Field), 0);
                    TestEqual(TEXT("Both generations own independent replacement Tasks"), Owner->GetTaskResultsForTesting().GetCount(), 6);
                    TestTrue(*Label, Owner->GetPreparedCount() > 0);
                    TestEqual(TEXT("Both prepared child cancellations remain unpublished"),
                        Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared), Mode == 0 ? 0 : 2);
                    TestEqual(TEXT("Preparation preserves old typed roots"), Original->GetManagedHeapForTesting()->GetStats().LiveRoots, OldRoots);
                });
            const bool Reloaded = Session.ReloadModule(Next.Bytes.GetData(), Next.Bytes.Num(), Next.Manifest, Reload);
            if (!TestEqual(TEXT("Public reload transaction result"), Reloaded, !Reject))
            { AddError(Label + TEXT(": ") + Reload.ErrorMessage); return false; }
            TestEqual(*Label, Observed, 1);
            TestTrue(*Label, Reload.bHostEffectTransactionAttempted && Reload.bStateMigrationApplied);
            TestEqual(*Label, Reload.bHostEffectTransactionCommitted, !Reject);
            TestEqual(*Label, Reload.bHostEffectRollbackSucceeded, Reject);
            TestEqual(*Label, Reload.bRollbackPreservedLiveRuntime, Reject);
            TestEqual(TEXT("Old VM survives only rollback"), OriginalLease.IsValid(), Reject);
            TestEqual(TEXT("Candidate VM survives only commit"), CandidateLease.IsValid(), !Reject);
            TestEqual(*Label, Session.GetSuccessfulReloadCount(), Reject ? 0 : 1);
            TestEqual(*Label, Session.GetRejectedReloadCount(), Reject ? 1 : 0);
            auto* Survivor = Session.GetLiveRuntimeForTesting();
            if (Reject)
            {
                TestTrue(TEXT("Rollback retains the actual original VM"), Survivor == Original);
                TestEqual(*Label, Reload.ErrorCategory, Backend == EAvidScriptVmBackendKind::Wasmtime ? FString(TEXT("guest_trap")) : FString(TEXT("trap")));
                TestEqual(*Label, Owner->GetStateFrameByteCountForTesting(), OldFrames);
                TestEqual(*Label, Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active), OldReady);
                TestEqual(*Label, Original->GetManagedHeapForTesting()->GetStats().LiveRoots, OldRoots);
            }
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
                TestEqual(*Label, Read(*Survivor, Fields[Index]), Reject ? Frozen[Index] : 0);
            TestEqual(TEXT("Rollback never reruns old BeginPlay"), ReadEntry(*Survivor, TEXT("BeginCount")), Reject ? 1 : 2);
            TestFalse(*Label, Session.GetSnapshot().bFaultQuarantined);
            TestEqual(*Label, Owner->GetTaskResultsForTesting().GetCount(), 3);
            TestEqual(*Label, Owner->GetCancellationSourceCountForTesting(), 1);
            TestEqual(*Label, Owner->GetPreparedCount(), 0);
            TestEqual(*Label, Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared), 0);
            for (int32 Round = 0; Round < 16 && Owner->GetActiveCount() > 0; ++Round)
            {
                World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
                if (!Session.TickLive(0.02f, Call)) { AddError(Label + TEXT(": ") + Call.ErrorMessage); return false; }
                TestTrue(*Label, Survivor->GetManagedHeapForTesting()->Collect() == AvidScript::Managed::EHeapError::Ok);
            }
            const bool Cancelled = Reject ? Stage >= 0 : Mode == 1;
            Expected = Cancelled ? 90 : Reject ? 33 : 65;
            TestEqual(TEXT("Correct source generation completes"), ReadEntry(*Survivor, TEXT("Result")), Expected);
            TestEqual(*Label, Read(*Survivor, TEXT("ChildFinally")), 2);
            TestEqual(*Label, Read(*Survivor, TEXT("InnerCatch")), Cancelled ? 2 : 0);
            TestEqual(*Label, Read(*Survivor, TEXT("OuterCatch")), Cancelled ? 1 : 0);
            TestEqual(*Label, Read(*Survivor, TEXT("OuterFinally")), 1);
            TestEqual(*Label, Read(*Survivor, TEXT("Iterations")), Cancelled ? 0 : 2);
            TestEqual(*Label, Read(*Survivor, TEXT("AfterWait")), 1);
            CheckHeapReleased(*this, *Survivor);
            CheckEmpty(*this, *Owner, 1);
        }
        TestTrue(*Label, Session.StopAndUnload(Call));
        TestTrue(*Label, Session.StopAndUnload(Call));
        CheckEmpty(*this, *Owner, 0);
        TestFalse(*Label, OriginalLease.IsValid());
        TestFalse(*Label, CandidateLease.IsValid());
        if (HasAnyErrors()) return false;
        ++Cases;
        AddInfo(FString::Printf(TEXT("task lifetime lifecycle %s result=%d resources=0 runtimes=0"), *Label, Expected));
    }
    TestEqual(TEXT("Task lifetime lifecycle scenario count"), Cases, 48);
    AddInfo(TEXT("TaskLocalLifetimeLifecycle: 48/48 passed"));
    return true;
}

#endif
