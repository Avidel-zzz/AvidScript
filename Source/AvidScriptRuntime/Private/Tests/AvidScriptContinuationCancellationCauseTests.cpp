#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptSessionContinuations.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "Async/Async.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace AvidScript::Tests::CancellationCause
{
struct FWorldScope
{
    UWorld* World = nullptr;
    FWorldScope()
    {
        if (!GEngine) return;
        World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("CancellationCauseWorld"));
        if (!World) return;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
    }
    ~FWorldScope() { Destroy(); }
    void Destroy()
    {
        if (!World) return;
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        World = nullptr;
    }
    void Tick()
    {
        // The first tick activates newly registered pending timers.
        World->Tick(LEVELTICK_All, 0.0f);
        ++GFrameCounter;
        World->Tick(LEVELTICK_All, 0.02f);
        ++GFrameCounter;
    }
};

static bool Drain(FAutomationTestBase& Test, FAvidScriptSessionContinuations& Owner,
    int64 Token, EAvidScriptContinuationStatus Status = EAvidScriptContinuationStatus::Cancelled)
{
    TArray<FAvidScriptContinuationCompletion> Ready;
    Owner.DrainReady(Ready);
    return Test.TestEqual(TEXT("Exactly one completion is selected"), Ready.Num(), 1)
        && Test.TestEqual(TEXT("The selected continuation retains its identity"), Ready[0].Token, Token)
        && Test.TestEqual(TEXT("Completion status"), Ready[0].Status, Status);
}

static void Reject(FAutomationTestBase& Test, const IAvidScriptContinuationHost& Host, int64 Token,
    const TCHAR* Context = TEXT("invalid dispatch"))
{
    int64 Cause = 123;
    Test.TestFalse(FString::Printf(TEXT("Cause query rejects %s (continuation=%lld)"), Context, Token),
        Host.ReadCancellationCause(Token, Cause));
    Test.TestEqual(TEXT("Rejected query clears stale output"), Cause, 0LL);
}

static void Clean(FAutomationTestBase& Test, FAvidScriptSessionContinuations& Owner)
{
    Owner.Teardown();
    Owner.Teardown();
    Test.TestEqual(TEXT("No continuation remains"), Owner.GetActiveCount(), 0);
    Test.TestEqual(TEXT("No source remains"), Owner.GetCancellationSourceCountForTesting(), 0);
    Test.TestEqual(TEXT("No reverse binding remains"), Owner.GetCancellationBindingCountForTesting(), 0);
    Test.TestEqual(TEXT("No saved state remains"), Owner.GetStateFrameByteCountForTesting(), 0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptCancellationCauseCaptureTest,
    "AvidScript.Runtime.Continuation.CancellationCauseCapture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCancellationCauseCaptureTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::CancellationCause;
    FWorldScope Scope;
    if (!TestNotNull(TEXT("Cause world"), Scope.World)) return false;
    const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
    auto& Host = Owner->ResetActive(Scope.World);
    int64 PreviousSource = 0;
    for (bool PreCancelled : {false, true})
    {
        const int64 Source = Host.CreateCancellationSource();
        TestTrue(TEXT("Source identity preserves its high bit"), Source < 0);
        TestNotEqual(TEXT("Source slot reuse changes its generation"), Source, PreviousSource);
        const int64 Timer = Host.ScheduleDelayWithCancelResume(10.0f, 17);
        const TArray<uint8> State{3, 7, 11};
        TestTrue(TEXT("Await state is stored"), Host.StoreState(Timer, State));
        Reject(*this, Host, Timer);
        if (PreCancelled) TestTrue(TEXT("Source cancels before binding"), Host.CancelCancellationSource(Source));
        TestTrue(TEXT("Timer binds"), Host.BindCancellationSource(Source, Timer));
        if (!PreCancelled) TestTrue(TEXT("Bound source cancels"), Host.CancelCancellationSource(Source));
        Reject(*this, Host, Timer);
        TestEqual(TEXT("Cancellation detaches all reverse bindings"), Owner->GetCancellationBindingCountForTesting(), 0);
        TestTrue(TEXT("Source releases before dispatch"), Host.ReleaseCancellationSource(Source));
        const int64 Replacement = Host.CreateCancellationSource();
        TestNotEqual(TEXT("Reused source is distinct"), Replacement, Source);
        if (!Drain(*this, *Owner, Timer)) return false;
        for (int32 Repeat = 0; Repeat < 4; ++Repeat)
        {
            int64 Cause = 0;
            TestTrue(TEXT("Cancelled await exposes its source identity"), Host.ReadCancellationCause(Timer, Cause));
            TestEqual(TEXT("Released source identity is stable"), Cause, Source);
        }
        TestEqual(TEXT("Observation does not retain the old source"), Owner->GetCancellationSourceCountForTesting(), 1);
        TestEqual(TEXT("Observation does not cancel a reused slot"), Host.GetCancellationSourceStatus(Replacement),
            EAvidScriptCancellationSourceStatus::Open);
        uint8 Readback[3] = {};
        TestTrue(TEXT("Cause reads do not consume continuation state"), Host.ReadState(Timer, MakeArrayView(Readback)));
        TestTrue(TEXT("Saved state bytes remain intact"), FMemory::Memcmp(Readback, State.GetData(), 3) == 0);
        TestTrue(TEXT("Cancellation dispatch finalizes"), Owner->FinalizeDispatched(Timer, true));
        Reject(*this, Host, Timer);
        TestTrue(TEXT("Replacement source releases"), Host.ReleaseCancellationSource(Replacement));
        PreviousSource = Source;
    }
    TestEqual(TEXT("Completed observations leave no continuation"), Owner->GetActiveCount(), 0);
    TestEqual(TEXT("Completed observations leave no saved state"), Owner->GetStateFrameByteCountForTesting(), 0);
    Clean(*this, *Owner);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptCancellationCauseWinnerTest,
    "AvidScript.Runtime.Continuation.CancellationCauseWinner",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCancellationCauseWinnerTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::CancellationCause;
    FWorldScope Scope;
    if (!TestNotNull(TEXT("Cause world"), Scope.World)) return false;
    const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
    auto& Host = Owner->ResetActive(Scope.World);
    const int64 First = Host.CreateCancellationSource(), Second = Host.CreateCancellationSource();
    const int64 Unrelated = Host.CreateCancellationSource();
    const int64 Manual = Host.ScheduleDelayWithCancelResume(10.0f, 21);
    TestTrue(TEXT("Manual cancellation target binds to an open source"), Host.BindCancellationSource(First, Manual));
    TestTrue(TEXT("Direct cancellation wins"), Host.Cancel(Manual));
    TestFalse(TEXT("An open source cannot bind to a queued cancellation"), Host.BindCancellationSource(Unrelated, Manual));
    TestEqual(TEXT("Rejected bind does not add a reverse binding"), Owner->GetCancellationBindingCountForTesting(), 0);
    TestTrue(TEXT("Later source cancellation succeeds independently"), Host.CancelCancellationSource(First));
    TestTrue(TEXT("Another source cancels independently"), Host.CancelCancellationSource(Second));
    TestFalse(TEXT("Rebinding cannot replace the winning cancellation"), Host.BindCancellationSource(Second, Manual));
    if (!Drain(*this, *Owner, Manual)) return false;
    int64 Cause = 123;
    TestTrue(TEXT("Manual cancellation has a readable empty cause"), Host.ReadCancellationCause(Manual, Cause));
    TestEqual(TEXT("An attached but non-winning source is not the cause"), Cause, 0LL);
    TestTrue(TEXT("Manual cancellation finalizes"), Owner->FinalizeDispatched(Manual, true));

    const int64 FromSource = Host.ScheduleDelayWithCancelResume(10.0f, 22);
    TestTrue(TEXT("Already-cancelled source wins at bind"), Host.BindCancellationSource(Second, FromSource));
    TestFalse(TEXT("Direct cancellation cannot replace source cancellation"), Host.Cancel(FromSource));
    TestFalse(TEXT("Another cancelled source cannot replace the cause"), Host.BindCancellationSource(First, FromSource));
    if (!Drain(*this, *Owner, FromSource)) return false;
    TestTrue(TEXT("Winning source is readable"), Host.ReadCancellationCause(FromSource, Cause));
    TestEqual(TEXT("The first winning source is retained"), Cause, Second);
    TestTrue(TEXT("Source cancellation finalizes"), Owner->FinalizeDispatched(FromSource, true));

    const int64 Open = Host.CreateCancellationSource();
    const int64 Completed = Host.ScheduleDelayWithCancelResume(0.001f, 23);
    TestTrue(TEXT("Normal timer binds"), Host.BindCancellationSource(Open, Completed));
    Scope.Tick();
    if (!Drain(*this, *Owner, Completed, EAvidScriptContinuationStatus::Completed)) return false;
    TestTrue(TEXT("Source can cancel after normal dispatch started"), Host.CancelCancellationSource(Open));
    Reject(*this, Host, Completed);
    TestTrue(TEXT("Normal dispatch stays normal"), Owner->FinalizeDispatched(Completed, true));

    const int64 Pending = Host.CreateCancellationSource();
    const int64 Ready = Host.ScheduleDelayWithCancelResume(0.001f, 24);
    TestTrue(TEXT("Ready timer binds"), Host.BindCancellationSource(Pending, Ready));
    Scope.Tick();
    TestTrue(TEXT("Source cancellation wins before a ready callback is selected"), Host.CancelCancellationSource(Pending));
    if (!Drain(*this, *Owner, Ready)) return false;
    TestTrue(TEXT("Replaced ready completion records its cause"), Host.ReadCancellationCause(Ready, Cause));
    TestEqual(TEXT("Ready completion identifies the winning source"), Cause, Pending);
    TestTrue(TEXT("Replaced completion finalizes"), Owner->FinalizeDispatched(Ready, true));
    Clean(*this, *Owner);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptCancellationCauseActivationTest,
    "AvidScript.Runtime.Continuation.CancellationCauseActivation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCancellationCauseActivationTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::CancellationCause;
    FWorldScope Scope;
    if (!TestNotNull(TEXT("Cause world"), Scope.World)) return false;
    const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
    auto& Active = Owner->ResetActive(Scope.World);
    FAvidScriptContinuationHostEndpoint OldAlias(Owner, EAvidScriptContinuationLane::Active, Active.GetActivationSerial());
    const int64 ActiveSource = Active.CreateCancellationSource();
    const int64 ActiveTimer = Active.ScheduleDelayWithCancelResume(10.0f, 31);
    TestTrue(TEXT("Active timer binds"), Active.BindCancellationSource(ActiveSource, ActiveTimer));
    TestTrue(TEXT("Active source cancels"), Active.CancelCancellationSource(ActiveSource));
    auto& Failed = Owner->BeginPrepared(Scope.World);
    FAvidScriptContinuationHostEndpoint FailedAlias(Owner, EAvidScriptContinuationLane::Prepared, Failed.GetActivationSerial());
    const int64 FailedSource = Failed.CreateCancellationSource();
    const int64 FailedTimer = Failed.ScheduleDelayWithCancelResume(10.0f, 32);
    TestTrue(TEXT("Candidate timer binds"), Failed.BindCancellationSource(FailedSource, FailedTimer));
    TestTrue(TEXT("Candidate source cancels"), Failed.CancelCancellationSource(FailedSource));
    Reject(*this, Active, FailedTimer);
    Reject(*this, Failed, ActiveTimer);
    Owner->DiscardPrepared();
    Reject(*this, FailedAlias, FailedTimer);
    if (!Drain(*this, *Owner, ActiveTimer)) return false;
    int64 Cause = 0;
    TestTrue(TEXT("Rollback preserves active cause"), Active.ReadCancellationCause(ActiveTimer, Cause));
    TestEqual(TEXT("Rollback preserves the original source"), Cause, ActiveSource);
    TestTrue(TEXT("Active callback finalizes after rollback"), Owner->FinalizeDispatched(ActiveTimer, true));

    const int64 RetiredTimer = Active.ScheduleDelayWithCancelResume(10.0f, 33);
    TestTrue(TEXT("Old activation queues another cancellation"), Active.BindCancellationSource(ActiveSource, RetiredTimer));
    auto& Published = Owner->BeginPrepared(Scope.World);
    FAvidScriptContinuationHostEndpoint PreparedAlias(Owner, EAvidScriptContinuationLane::Prepared, Published.GetActivationSerial());
    const int64 PublishedSource = Published.CreateCancellationSource();
    const int64 PublishedTimer = Published.ScheduleDelayWithCancelResume(10.0f, 34);
    TestTrue(TEXT("Published candidate binds"), Published.BindCancellationSource(PublishedSource, PublishedTimer));
    TestTrue(TEXT("Published candidate cancels"), Published.CancelCancellationSource(PublishedSource));
    FString Error;
    if (!TestTrue(TEXT("Candidate is valid for publication"), Owner->ValidatePreparedCommit(Error))) return false;
    Owner->CommitPrepared();
    Reject(*this, OldAlias, RetiredTimer);
    Reject(*this, OldAlias, PublishedTimer);
    Reject(*this, PreparedAlias, PublishedTimer);
    if (!Drain(*this, *Owner, PublishedTimer)) return false;
    TestTrue(TEXT("Promoted endpoint can read its cancellation"), Published.ReadCancellationCause(PublishedTimer, Cause));
    TestEqual(TEXT("Promotion does not rewrite source identity"), Cause, PublishedSource);
    TestTrue(TEXT("Published cancellation finalizes"), Owner->FinalizeDispatched(PublishedTimer, true));
    FAvidScriptContinuationHostEndpoint PublishedAlias(Owner, EAvidScriptContinuationLane::Active, Published.GetActivationSerial());
    Clean(*this, *Owner);
    Reject(*this, PublishedAlias, PublishedTimer);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptCancellationCauseContextTest,
    "AvidScript.Runtime.Continuation.CancellationCauseContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCancellationCauseContextTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::CancellationCause;
    FWorldScope Scope;
    if (!TestNotNull(TEXT("Cause world"), Scope.World)) return false;
    const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
    const auto Foreign = MakeShared<FAvidScriptSessionContinuations>();
    FAvidScriptObjectRegistry Registry;
    FAvidScriptSessionObjectOwnership Ownership;
    UObject* Object = NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
    FAvidScriptObjectHandleResult Result;
    const auto Handle = Registry.RegisterObject(Object, Result, false);
    auto& Host = Owner->ResetActive(Scope.World, &Registry, &Ownership, Handle);
    auto& ForeignHost = Foreign->ResetActive(Scope.World);
    const int64 ForeignTimer = ForeignHost.ScheduleDelayWithCancelResume(10.0f, 40);
    TestTrue(TEXT("Foreign timer cancels"), ForeignHost.Cancel(ForeignTimer));
    if (!Drain(*this, *Foreign, ForeignTimer)) return false;
    const int64 Source = Host.CreateCancellationSource();
    const int64 Timer = Host.ScheduleDelayWithCancelResume(10.0f, 41);
    const int64 Task = Host.CreateTaskResult(TEXT("System.Int32"));
    TestNotEqual(TEXT("A task token exists for kind validation"), Task, 0LL);
    TestTrue(TEXT("Owned timer binds"), Host.BindCancellationSource(Source, Timer));
    TestTrue(TEXT("Owned source cancels"), Host.CancelCancellationSource(Source));
    if (!Drain(*this, *Owner, Timer)) return false;
    TestNotEqual(TEXT("Two live sessions cannot share continuation identity"), Timer, ForeignTimer);
    Reject(*this, ForeignHost, Timer, TEXT("a foreign Session with an active cancellation dispatch"));
    Reject(*this, Host, ForeignTimer, TEXT("a foreign continuation"));
    Reject(*this, Host, 0);
    Reject(*this, Host, Source);
    Reject(*this, Host, Task);
    Reject(*this, Host, Timer ^ (1LL << 32));
    TestTrue(TEXT("Off-thread reads fail without retaining or touching owner state"),
        Async(EAsyncExecution::ThreadPool, [&Host, Timer] {
            int64 Cause = 123;
            return !Host.ReadCancellationCause(Timer, Cause) && Cause == 0;
        }).Get());
    TestTrue(TEXT("The unrelated task releases"), Host.ReleaseTaskResult(Task));
    TestTrue(TEXT("Owner handle invalidates"), Registry.ReleaseHandle(Handle, Result, false));
    Reject(*this, Host, Timer, TEXT("a released owner handle"));
    Clean(*this, *Owner);

    auto& Live = Owner->ResetActive(Scope.World);
    const int64 WorldSource = Live.CreateCancellationSource();
    const int64 WorldTimer = Live.ScheduleDelayWithCancelResume(10.0f, 42);
    TestTrue(TEXT("World timer binds"), Live.BindCancellationSource(WorldSource, WorldTimer));
    TestTrue(TEXT("World source cancels"), Live.CancelCancellationSource(WorldSource));
    if (!Drain(*this, *Owner, WorldTimer)) return false;
    const int64 Queued = Live.ScheduleDelayWithCancelResume(10.0f, 43);
    TestTrue(TEXT("Another World cancellation is queued"), Live.BindCancellationSource(WorldSource, Queued));
    Scope.Destroy();
    Reject(*this, Live, WorldTimer, TEXT("a destroyed World before GC"));
    TestEqual(TEXT("Destroyed World rejects source status"), Live.GetCancellationSourceStatus(WorldSource),
        EAvidScriptCancellationSourceStatus::Invalid);
    TestEqual(TEXT("Destroyed World rejects new timers"), Live.ScheduleDelayWithCancelResume(1.0f, 44), 0LL);
    TArray<FAvidScriptContinuationCompletion> Ready;
    Owner->DrainReady(Ready);
    TestEqual(TEXT("Destroyed World cannot dispatch its queued callback"), Ready.Num(), 0);
    TestEqual(TEXT("Destroyed World releases active and queued continuations"), Owner->GetActiveCount(), 0);
    TestEqual(TEXT("Destroyed World releases cancellation sources"), Owner->GetCancellationSourceCountForTesting(), 0);
    Clean(*this, *Owner);
    Clean(*this, *Foreign);
    return true;
}

#endif
