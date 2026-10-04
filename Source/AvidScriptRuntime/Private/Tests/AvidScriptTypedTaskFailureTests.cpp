#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <array>

namespace AvidScriptTypedTaskFailureTestPrivate
{
using namespace AvidScript::Managed;
class FErrorLease final : public IAvidScriptTaskLanguageErrorLease
{
public:
    explicit FErrorLease(FPersistentRoots&& InRoots) : Roots(MoveTemp(InRoots)) {}
private:
    FPersistentRoots Roots;
};
static TSharedPtr<IAvidScriptTaskLanguageErrorLease> MakeLease(
    FAutomationTestBase& Test, FHeap& Heap, FToken& Parent, FToken& Child)
{
    FToken Frame = 0, ParentRoot = 0, ChildRoot = 0;
    if (!Test.TestTrue(TEXT("Error producer frame starts"), Heap.PushFrame(Frame) == EHeapError::Ok)) return nullptr;
    FPersistentRoots Roots;
    const bool bValid = Test.TestTrue(TEXT("Error parent root exists"), Heap.CreateRoot(Frame, 0, ParentRoot) == EHeapError::Ok)
        && Test.TestTrue(TEXT("Error child root exists"), Heap.CreateRoot(Frame, 0, ChildRoot) == EHeapError::Ok)
        && Test.TestTrue(TEXT("Error parent allocates"), Heap.Allocate(1, ParentRoot, Parent) == EHeapError::Ok)
        && Test.TestTrue(TEXT("Error child allocates"), Heap.Allocate(1, ChildRoot, Child) == EHeapError::Ok)
        && Test.TestTrue(TEXT("Error graph links"), Heap.WriteReference(Parent, 1, 0, Child) == EHeapError::Ok)
        && Test.TestTrue(TEXT("Error persistent proof acquired"), Heap.RetainPersistent({&Parent, 1}, Roots) == EHeapError::Ok);
    Test.TestTrue(TEXT("Error producer exits"), Heap.PopFrame(Frame) == EHeapError::Ok);
    if (bValid) return MakeShared<FErrorLease>(MoveTemp(Roots));
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTypedTaskFailureIdentityTest,
    "AvidScript.Runtime.Continuation.TypedTaskFailure.IdentityAndReaders",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedTaskFailureIdentityTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Managed;
    FHeap Heap; const std::array<FHeapLayout, 1> Layouts{{{1, 8, {{0, 1}}}}};
    if (!TestTrue(TEXT("Error heap configures"), Heap.Configure(Layouts) == EHeapError::Ok)) return false;
    FToken Parent = 0, Child = 0;
    auto Lease = AvidScriptTypedTaskFailureTestPrivate::MakeLease(*this, Heap, Parent, Child);
    if (!Lease) return false;
    TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
    const auto Owner = MakeShared<FAvidScriptSessionContinuations>(); auto& Host = Owner->ResetActive(World.Get());
    const int64 Source = Host.CreateTaskResult(TEXT("test:Int")), Target = Host.CreateRootedTaskResult(TEXT("test:Node"));
    int64 First = 0, Second = 0;
    TestTrue(TEXT("First typed failure reader queues"), Host.AwaitTaskResult(Target, 621, First) == EAvidScriptTaskWaitRegistration::Queued);
    TestTrue(TEXT("Second typed failure reader queues"), Host.AwaitTaskResult(Target, 622, Second) == EAvidScriptTaskWaitRegistration::Queued);
    const FAvidScriptTaskLanguageError Error{3, 7, Parent}; TArray<int64> Woken;
    TestTrue(TEXT("Source acquires language error"), Host.FaultTaskResultLanguageError(Source, Error, MoveTemp(Lease), Woken));
    Woken.Add(12345);
    TestFalse(TEXT("Wrong source identity rejected"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Float"), Target, TEXT("test:Node"), Woken));
    TestFalse(TEXT("Wrong target identity rejected"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Target, TEXT("test:Float"), Woken));
    TestFalse(TEXT("Missing identity rejected"), Host.PropagateTypedTaskFailure(Source, TEXT(""), Target, TEXT("test:Node"), Woken));
    TestFalse(TEXT("Legacy cross-type propagation remains rejected"), Host.PropagateTaskFailure(Source, Target, Woken));
    TestTrue(TEXT("Rejected propagation preserves waiter output"), Woken.Num() == 1 && Woken[0] == 12345);
    TestEqual(TEXT("Rejected propagation preserves registered waiters"), Owner->GetTaskResultsForTesting().GetWaiterCount(), 2);
    TestTrue(TEXT("Explicit types authorize different result declarations"),
        Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Target, TEXT("test:Node"), Woken));
    TestTrue(TEXT("Typed propagation preserves registration order"), Woken.Num() == 2 && Woken[0] == First && Woken[1] == Second);
    TestFalse(TEXT("Terminal target cannot complete twice"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Target, TEXT("test:Node"), Woken));
    FAvidScriptTaskResultSnapshot Snapshot;
    TestTrue(TEXT("Target retains original declaration and fault"), Host.ReadTaskResult(Target, Snapshot)
        && Snapshot.TypeId == TEXT("test:Node") && Snapshot.bValueRequiresLease
        && Snapshot.Value.IsEmpty() && Snapshot.State == EAvidScriptTaskResultState::Faulted && Snapshot.ErrorCode == TEXT("language_error"));
    TestTrue(TEXT("Original error identity and provenance remain"), Snapshot.LanguageError.IsSet()
        && Snapshot.LanguageError->TypeToken == 3 && Snapshot.LanguageError->SourceToken == 7 && Snapshot.LanguageError->ObjectToken == Parent);
    TestTrue(TEXT("Source releases"), Host.ReleaseTaskResult(Source));
    TestTrue(TEXT("Target caller releases"), Host.ReleaseTaskResult(Target));
    TestTrue(TEXT("Target waiters retain entire source error graph"), Heap.Collect() == EHeapError::Ok && Heap.IsAlive(Parent) && Heap.IsAlive(Child));
    TArray<FAvidScriptContinuationCompletion> Ready; Owner->DrainReady(Ready);
    TestTrue(TEXT("First failure callback dispatches first"), Ready.Num() == 1 && Ready[0].Token == First);
    TestTrue(TEXT("First failure reader finalizes"), Owner->FinalizeDispatched(First, true));
    TestTrue(TEXT("Second reader still owns error graph"), Heap.Collect() == EHeapError::Ok && Heap.IsAlive(Parent));
    Owner->DrainReady(Ready);
    TestTrue(TEXT("Second failure callback dispatches second"), Ready.Num() == 1 && Ready[0].Token == Second);
    TestTrue(TEXT("Last failure reader finalizes"), Owner->FinalizeDispatched(Second, true));
    TestTrue(TEXT("Copied snapshot does not retain error graph"), Heap.Collect() == EHeapError::Ok && Heap.GetStats().LiveObjects == 0);
    Owner->Teardown(); TestEqual(TEXT("All error roots released"), Heap.GetStats().LiveRoots, 0u);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTypedTaskFailureAdmissionTest,
    "AvidScript.Runtime.Continuation.TypedTaskFailure.Admission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedTaskFailureAdmissionTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
    const auto Owner = MakeShared<FAvidScriptSessionContinuations>(), ForeignOwner = MakeShared<FAvidScriptSessionContinuations>();
    auto& Host = Owner->ResetActive(World.Get()); auto& Foreign = ForeignOwner->ResetActive(World.Get());
    const int64 Running = Host.CreateTaskResult(TEXT("test:Int")), Succeeded = Host.CreateTaskResult(TEXT("test:Int"));
    const int64 Source = Host.CreateTaskResult(TEXT("test:Int")), Target = Host.CreateRootedTaskResult(TEXT("test:Int"));
    TArray<int64> Woken; const std::array<uint8, 4> Bytes{{1, 0, 0, 0}};
    TestTrue(TEXT("Successful source finishes"), Host.SucceedTaskResult(Succeeded, {Bytes.data(), 4}, Woken));
    TestFalse(TEXT("Running source is not a failure"), Host.PropagateTypedTaskFailure(Running, TEXT("test:Int"), Target, TEXT("test:Int"), Woken));
    TestFalse(TEXT("Successful source is not a failure"), Host.PropagateTypedTaskFailure(Succeeded, TEXT("test:Int"), Target, TEXT("test:Int"), Woken));
    TestTrue(TEXT("Native failure source finishes"), Host.FaultTaskResult(Source, TEXT("guest_failure"), Woken));
    TestFalse(TEXT("Self propagation is invalid"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Source, TEXT("test:Int"), Woken));
    TestFalse(TEXT("Legacy lease declaration mismatch rejected"), Host.PropagateTaskFailure(Source, Target, Woken));
    const int64 ForeignTask = Foreign.CreateTaskResult(TEXT("test:Int"));
    TestTrue(TEXT("Foreign source faults"), Foreign.FaultTaskResult(ForeignTask, TEXT("foreign_failure"), Woken));
    TestFalse(TEXT("Foreign source denied"), Host.PropagateTypedTaskFailure(ForeignTask, TEXT("test:Int"), Target, TEXT("test:Int"), Woken));
    TestFalse(TEXT("Foreign target denied"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), ForeignTask, TEXT("test:Int"), Woken));
    auto& Prepared = Owner->BeginPrepared(World.Get());
    const int64 Candidate = Prepared.CreateRootedTaskResult(TEXT("test:Node"));
    TestFalse(TEXT("Active cannot complete candidate"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Candidate, TEXT("test:Node"), Woken));
    TestFalse(TEXT("Candidate cannot use active source"), Prepared.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Candidate, TEXT("test:Node"), Woken));
    Owner->DiscardPrepared();
    TestFalse(TEXT("Retired candidate token remains invalid"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Candidate, TEXT("test:Node"), Woken));
    TestTrue(TEXT("Explicit identities allow failure across lease declarations"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Target, TEXT("test:Int"), Woken));
    FAvidScriptTaskResultSnapshot Snapshot;
    TestTrue(TEXT("Plain error code and rooted target declaration preserved"), Host.ReadTaskResult(Target, Snapshot)
        && Snapshot.ErrorCode == TEXT("guest_failure") && Snapshot.bValueRequiresLease && !Snapshot.LanguageError.IsSet());
    FAvidScriptSessionTaskResults Tasks;
    const int64 Old = Tasks.Create(EAvidScriptContinuationLane::Active, 8, TEXT("test:Int"));
    const int64 OtherActivation = Tasks.Create(EAvidScriptContinuationLane::Active, 9, TEXT("test:Node"), true);
    const int64 OtherLane = Tasks.Create(EAvidScriptContinuationLane::Prepared, 8, TEXT("test:Node"), true);
    TestTrue(TEXT("Store source faults"), Tasks.Fault(Old, TEXT("guest_failure"), Woken));
    TestFalse(TEXT("Same lane different activation denied by store"), Tasks.PropagateTypedFailure(Old, TEXT("test:Int"), OtherActivation, TEXT("test:Node"), Woken));
    TestFalse(TEXT("Same activation different lane denied by store"), Tasks.PropagateTypedFailure(Old, TEXT("test:Int"), OtherLane, TEXT("test:Node"), Woken));
    Tasks.RetireLane(EAvidScriptContinuationLane::Active, 8); Tasks.RetireLane(EAvidScriptContinuationLane::Active, 9);
    Tasks.RetireLane(EAvidScriptContinuationLane::Prepared, 8);
    Owner->Teardown(); ForeignOwner->Teardown();
    TestFalse(TEXT("Teardown forbids new propagation"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Int"), Target, TEXT("test:Int"), Woken));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTypedTaskFailureCancellationTest,
    "AvidScript.Runtime.Continuation.TypedTaskFailure.CancellationAndRetirement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedTaskFailureCancellationTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Managed;
    FHeap Heap; const std::array<FHeapLayout, 1> Layouts{{{1, 8, {{0, 1}}}}};
    if (!TestTrue(TEXT("Cancellation heap configures"), Heap.Configure(Layouts) == EHeapError::Ok)) return false;
    FToken Parent = 0, Child = 0; auto Lease = AvidScriptTypedTaskFailureTestPrivate::MakeLease(*this, Heap, Parent, Child);
    if (!Lease) return false;
    TStrongObjectPtr<UWorld> World(NewObject<UWorld>()); const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
    auto& Host = Owner->ResetActive(World.Get()); const int64 Source = Host.CreateRootedTaskResult(TEXT("test:Node"));
    const int64 Target = Host.CreateTaskResult(TEXT("test:Float"));
    FAvidScriptTaskLanguageError Error{4, 9, Parent}; Error.CancellationSourceToken = -98765;
    TArray<int64> Woken;
    TestTrue(TEXT("Rooted result cancels with original identity"), Host.CancelTaskResultLanguageError(Source, Error, MoveTemp(Lease), Woken));
    TestTrue(TEXT("Cancellation propagates to another result type"), Host.PropagateTypedTaskFailure(Source, TEXT("test:Node"), Target, TEXT("test:Float"), Woken));
    FAvidScriptTaskResultSnapshot Snapshot;
    TestTrue(TEXT("Target keeps cancellation and scalar declaration"), Host.ReadTaskResult(Target, Snapshot)
        && Snapshot.State == EAvidScriptTaskResultState::Cancelled && Snapshot.TypeId == TEXT("test:Float") && !Snapshot.bValueRequiresLease);
    TestTrue(TEXT("Cancellation error and source identity remain"), Snapshot.LanguageError.IsSet() && Snapshot.LanguageError->ObjectToken == Parent
        && Snapshot.LanguageError->TypeToken == 4 && Snapshot.LanguageError->SourceToken == 9
        && Snapshot.LanguageError->CancellationSourceToken.IsSet() && Snapshot.LanguageError->CancellationSourceToken.GetValue() == -98765);
    TestTrue(TEXT("Original cancelled task releases"), Host.ReleaseTaskResult(Source));
    TestTrue(TEXT("Propagated cancellation owns graph"), Heap.Collect() == EHeapError::Ok && Heap.IsAlive(Parent) && Heap.IsAlive(Child));
    Owner->Teardown(); Owner->Teardown();
    TestTrue(TEXT("Idempotent retirement releases propagated graph"), Heap.Collect() == EHeapError::Ok
        && Heap.GetStats().LiveObjects == 0 && Heap.GetStats().LiveRoots == 0);
    TestFalse(TEXT("Retired target cannot be read"), Host.ReadTaskResult(Target, Snapshot));
    return true;
}

#endif
