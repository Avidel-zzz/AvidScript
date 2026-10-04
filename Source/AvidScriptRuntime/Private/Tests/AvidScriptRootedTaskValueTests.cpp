#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include <array>

namespace AvidScriptRootedTaskValueTestPrivate
{
using namespace AvidScript::Managed;

class FValueLease final : public IAvidScriptTaskValueLease
{
public:
	explicit FValueLease(FPersistentRoots&& InRoots) : Roots(MoveTemp(InRoots)) {}
private:
	FPersistentRoots Roots;
};

static TSharedPtr<IAvidScriptTaskValueLease> MakeLease(
	FAutomationTestBase& Test, FHeap& Heap, FToken& Object)
{
	FToken Frame = 0, Root = 0;
	if (!Test.TestTrue(TEXT("Value frame starts"), Heap.PushFrame(Frame) == EHeapError::Ok))
		return nullptr;
	FPersistentRoots Roots;
	const bool bValid = Test.TestTrue(TEXT("Value frame root exists"),
		Heap.CreateRoot(Frame, 0, Root) == EHeapError::Ok)
		&& Test.TestTrue(TEXT("Value object allocates"),
			Heap.Allocate(1, Root, Object) == EHeapError::Ok)
		&& Test.TestTrue(TEXT("Value acquires persistent root"),
			Heap.RetainPersistent({&Object, 1}, Roots) == EHeapError::Ok);
	Test.TestTrue(TEXT("Value frame exits"), Heap.PopFrame(Frame) == EHeapError::Ok);
	if (bValid) return MakeShared<FValueLease>(MoveTemp(Roots));
	return nullptr;
}

static TConstArrayView<uint8> Bytes(const FToken& Object)
{
	return {reinterpret_cast<const uint8*>(&Object), sizeof(Object)};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRootedTaskValueResultsTest,
	"AvidScript.Runtime.Continuation.RootedTask.Results",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRootedTaskValueResultsTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	using namespace AvidScriptRootedTaskValueTestPrivate;
	FHeap Heap;
	const std::array<FHeapLayout, 2> Layouts{{{1, 8, {}}, {2, 8, {{0, 1}}}}};
	if (!TestTrue(TEXT("Value heap configures"), Heap.Configure(Layouts) == EHeapError::Ok)) return false;
	FToken Frame = 0, LeafRoot = 0, ParentRoot = 0, Leaf = 0, Parent = 0;
	TestTrue(TEXT("Producer frame starts"), Heap.PushFrame(Frame) == EHeapError::Ok);
	TestTrue(TEXT("Leaf frame root exists"), Heap.CreateRoot(Frame, 0, LeafRoot) == EHeapError::Ok);
	TestTrue(TEXT("Parent frame root exists"), Heap.CreateRoot(Frame, 0, ParentRoot) == EHeapError::Ok);
	TestTrue(TEXT("Leaf allocates"), Heap.Allocate(1, LeafRoot, Leaf) == EHeapError::Ok);
	TestTrue(TEXT("Parent allocates"), Heap.Allocate(2, ParentRoot, Parent) == EHeapError::Ok);
	TestTrue(TEXT("Parent owns nested reference"), Heap.WriteReference(Parent, 2, 0, Leaf) == EHeapError::Ok);
	FPersistentRoots Roots;
	TestTrue(TEXT("Successful value acquires parent root"), Heap.RetainPersistent({&Parent, 1}, Roots) == EHeapError::Ok);
	TestTrue(TEXT("Producer frame exits"), Heap.PopFrame(Frame) == EHeapError::Ok);
	TSharedPtr<IAvidScriptTaskValueLease> Lease = MakeShared<FValueLease>(MoveTemp(Roots));
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
	auto& Host = Owner->ResetActive(World.Get());
	const int64 Task = Host.CreateRootedTaskResult(TEXT("test:SharedPair"));
	int64 FirstWaiter = 0, SecondWaiter = 0;
	TestTrue(TEXT("First waiter queues"), Host.AwaitTaskResult(Task, 601, FirstWaiter) == EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Second waiter queues"), Host.AwaitTaskResult(Task, 602, SecondWaiter) == EAvidScriptTaskWaitRegistration::Queued);
	std::array<FToken, 2> Payload{{Parent, Parent}};
	const TConstArrayView<uint8> Value(reinterpret_cast<const uint8*>(Payload.data()), sizeof(Payload));
	TArray<int64> Woken;
	TestTrue(TEXT("Rooted task completes"), Host.SucceedRootedTaskResult(Task, Value, MoveTemp(Lease), Woken));
	TestFalse(TEXT("Caller no longer owns transferred lease"), Lease.IsValid());
	TestEqual(TEXT("Both waiters wake"), Woken.Num(), 2);
	Payload[0] = 0;
	FAvidScriptTaskResultSnapshot First, Second;
	TestTrue(TEXT("First snapshot reads"), Host.ReadTaskResult(Task, First));
	TestTrue(TEXT("Second snapshot reads"), Host.ReadTaskResult(Task, Second));
	TestTrue(TEXT("Snapshots agree on immutable bytes"), First.Value == Second.Value);
	TestTrue(TEXT("Snapshot declares reference lease"), First.bValueRequiresLease);
	FToken FirstField = 0, SecondField = 0;
	if (!TestEqual(TEXT("Pair has two tokens"), First.Value.Num(), static_cast<int32>(2 * sizeof(FToken)))) return false;
	FMemory::Memcpy(&FirstField, First.Value.GetData(), sizeof(FToken));
	FMemory::Memcpy(&SecondField, First.Value.GetData() + sizeof(FToken), sizeof(FToken));
	TestEqual(TEXT("Source mutation cannot change published value"), FirstField, Parent);
	TestEqual(TEXT("Shared object identity survives both fields"), SecondField, Parent);
	TestTrue(TEXT("Producer drops task reference"), Host.ReleaseTaskResult(Task));
	TestTrue(TEXT("Collection succeeds while waiters own task"), Heap.Collect() == EHeapError::Ok);
	TestTrue(TEXT("Parent survives frame exit"), Heap.IsAlive(Parent));
	TestTrue(TEXT("Nested leaf survives frame exit"), Heap.IsAlive(Leaf));
	TArray<FAvidScriptContinuationCompletion> Ready;
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Dispatcher selects one callback per drain"), Ready.Num(), 1);
	if (Ready.Num() == 1)
		TestEqual(TEXT("First callback preserves registration order"), Ready[0].CallbackId, 601);
	TestTrue(TEXT("First waiter finalizes"), Owner->FinalizeDispatched(FirstWaiter, true));
	TestTrue(TEXT("Collection succeeds with second waiter"), Heap.Collect() == EHeapError::Ok);
	TestTrue(TEXT("Second waiter still owns result tree"), Heap.IsAlive(Parent) && Heap.IsAlive(Leaf));
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Next drain selects remaining callback"), Ready.Num(), 1);
	if (Ready.Num() == 1)
		TestEqual(TEXT("Second callback preserves registration order"), Ready[0].CallbackId, 602);
	FToken ConsumerFrame = 0, ConsumerRoot = 0;
	TestTrue(TEXT("Consumer frame starts"), Heap.PushFrame(ConsumerFrame) == EHeapError::Ok);
	TestTrue(TEXT("Reader acquires its own root before task release"), Heap.CreateRoot(ConsumerFrame, FirstField, ConsumerRoot) == EHeapError::Ok);
	TestTrue(TEXT("Last waiter finalizes"), Owner->FinalizeDispatched(SecondWaiter, true));
	TestEqual(TEXT("Last task reference reclaims task"), Owner->GetTaskResultsForTesting().GetCount(), 0);
	TestFalse(TEXT("Old task identity cannot read again"), Host.ReadTaskResult(Task, Second));
	TestTrue(TEXT("Consumer root survives task retirement"), Heap.Collect() == EHeapError::Ok);
	TestTrue(TEXT("Consumer retains complete tree"), Heap.IsAlive(Parent) && Heap.IsAlive(Leaf));
	TestTrue(TEXT("Consumer frame exits"), Heap.PopFrame(ConsumerFrame) == EHeapError::Ok);
	TestTrue(TEXT("Unowned tree collects"), Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Copied snapshots do not keep parent alive"), Heap.IsAlive(Parent));
	TestFalse(TEXT("Copied snapshots do not keep leaf alive"), Heap.IsAlive(Leaf));
	TestEqual(TEXT("No roots remain"), Heap.GetStats().LiveRoots, 0u);
	Owner->Teardown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRootedTaskValueAdmissionTest,
	"AvidScript.Runtime.Continuation.RootedTask.Admission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRootedTaskValueAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	using namespace AvidScriptRootedTaskValueTestPrivate;
	FHeap Heap;
	const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
	if (!TestTrue(TEXT("Admission heap configures"), Heap.Configure(Layouts) == EHeapError::Ok)) return false;
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
	auto& Host = Owner->ResetActive(World.Get());
	const int64 Rooted = Host.CreateRootedTaskResult(TEXT("test:Reference"));
	int64 Waiter = 0;
	TestTrue(TEXT("Pending waiter queues"), Host.AwaitTaskResult(Rooted, 603, Waiter) == EAvidScriptTaskWaitRegistration::Queued);
	TArray<int64> Woken{777};
	FToken Object = 0;
	TestFalse(TEXT("Rooted value cannot use unrooted completion"), Host.SucceedTaskResult(Rooted, Bytes(Object), Woken));
	TestFalse(TEXT("Rooted completion rejects missing lease"), Host.SucceedRootedTaskResult(Rooted, Bytes(Object), nullptr, Woken));
	TestEqual(TEXT("Rejected completion preserves caller waiter output"), Woken[0], 777LL);
	TestEqual(TEXT("Rejected completion preserves pending waiter"), Owner->GetTaskResultsForTesting().GetWaiterCount(), 1);
	FAvidScriptTaskResultSnapshot Snapshot;
	TestFalse(TEXT("Rejected completion leaves task running"), Host.ReadTaskResult(Rooted, Snapshot));
	const int64 Plain = Host.CreateTaskResult(TEXT("test:Reference"));
	auto Lease = MakeLease(*this, Heap, Object);
	TestFalse(TEXT("Unrooted task rejects extra lease"), Host.SucceedRootedTaskResult(Plain, Bytes(Object), MoveTemp(Lease), Woken));
	TestTrue(TEXT("Rejected extra lease collects"), Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Rejected extra lease releases incoming root"), Heap.IsAlive(Object));
	Lease = MakeLease(*this, Heap, Object);
	TestFalse(TEXT("Rooted completion rejects empty payload"), Host.SucceedRootedTaskResult(Rooted, {}, MoveTemp(Lease), Woken));
	TestTrue(TEXT("Empty payload root collects"), Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Empty payload releases incoming root"), Heap.IsAlive(Object));
	TArray<uint8> Oversized;
	Oversized.SetNumZeroed(FAvidScriptSessionTaskResults::MaximumValueBytes + 1);
	Lease = MakeLease(*this, Heap, Object);
	TestFalse(TEXT("Oversized rooted payload rejects"), Host.SucceedRootedTaskResult(Rooted, Oversized, MoveTemp(Lease), Woken));
	TestTrue(TEXT("Oversized payload root collects"), Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Oversized payload releases incoming root"), Heap.IsAlive(Object));
	TestTrue(TEXT("Rooted task can fault without a successful value"), Host.FaultTaskResult(Rooted, TEXT("test_fault"), Woken));
	TestTrue(TEXT("Fault snapshot reads"), Host.ReadTaskResult(Rooted, Snapshot));
	TestTrue(TEXT("Fault has no successful value"), Snapshot.State == EAvidScriptTaskResultState::Faulted && Snapshot.Value.IsEmpty());
	TestTrue(TEXT("Fault retains declared value contract"), Snapshot.bValueRequiresLease);
	TestFalse(TEXT("Failure propagation rejects different value ownership"), Host.PropagateTaskFailure(Rooted, Plain, Woken));
	Lease = MakeLease(*this, Heap, Object);
	TestFalse(TEXT("Rooted value cannot overwrite fault"), Host.SucceedRootedTaskResult(Rooted, Bytes(Object), MoveTemp(Lease), Woken));
	TestTrue(TEXT("Duplicate completion root collects"), Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Duplicate completion releases incoming root"), Heap.IsAlive(Object));
	TestTrue(TEXT("Fault remains unchanged"), Host.ReadTaskResult(Rooted, Snapshot) && Snapshot.ErrorCode == TEXT("test_fault"));
	const int64 Cancelled = Host.CreateRootedTaskResult(TEXT("test:Reference"));
	TestTrue(TEXT("Rooted pending task can cancel"), Host.CancelTaskResult(Cancelled, Woken));
	TestTrue(TEXT("Cancelled value remains empty"), Host.ReadTaskResult(Cancelled, Snapshot)
		&& Snapshot.State == EAvidScriptTaskResultState::Cancelled && Snapshot.Value.IsEmpty());
	const int64 Unowned = Host.CreateRootedTaskResult(TEXT("test:Reference"));
	TestTrue(TEXT("Producer may release last running reference"), Host.ReleaseTaskResult(Unowned));
	Lease = MakeLease(*this, Heap, Object);
	TestTrue(TEXT("Unowned rooted completion succeeds and retires"), Host.SucceedRootedTaskResult(Unowned, Bytes(Object), MoveTemp(Lease), Woken));
	TestFalse(TEXT("Unowned terminal task is stale"), Host.ReadTaskResult(Unowned, Snapshot));
	TestTrue(TEXT("Unowned successful value collects"), Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Unowned completion releases value root immediately"), Heap.IsAlive(Object));
	Owner->Teardown();
	TestEqual(TEXT("Admission leaves no task records"), Owner->GetTaskResultsForTesting().GetCount(), 0);
	TestEqual(TEXT("Admission leaves no waiters"), Owner->GetTaskResultsForTesting().GetWaiterCount(), 0);
	TestEqual(TEXT("Admission leaves no roots"), Heap.GetStats().LiveRoots, 0u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRootedTaskValueRetirementTest,
	"AvidScript.Runtime.Continuation.RootedTask.Retirement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRootedTaskValueRetirementTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	using namespace AvidScriptRootedTaskValueTestPrivate;
	FHeap Heap;
	const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
	if (!TestTrue(TEXT("Retirement heap configures"), Heap.Configure(Layouts) == EHeapError::Ok)) return false;
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
	auto& Active = Owner->ResetActive(World.Get());
	const int64 Old = Active.CreateRootedTaskResult(TEXT("test:Reference"));
	FToken OldObject = 0, CandidateObject = 0, NewObject = 0;
	TArray<int64> Woken;
	auto Lease = MakeLease(*this, Heap, OldObject);
	TestTrue(TEXT("Old active result publishes"), Active.SucceedRootedTaskResult(Old, Bytes(OldObject), MoveTemp(Lease), Woken));
	auto& Rejected = Owner->BeginPrepared(World.Get());
	const int64 Candidate = Rejected.CreateRootedTaskResult(TEXT("test:Reference"));
	Lease = MakeLease(*this, Heap, CandidateObject);
	TestFalse(TEXT("Candidate endpoint rejects active identity"), Rejected.SucceedRootedTaskResult(Old, Bytes(CandidateObject), Lease, Woken));
	TestTrue(TEXT("Candidate result publishes"), Rejected.SucceedRootedTaskResult(Candidate, Bytes(CandidateObject), MoveTemp(Lease), Woken));
	Owner->DiscardPrepared();
	TestTrue(TEXT("Rollback collection succeeds"), Heap.Collect() == EHeapError::Ok);
	TestTrue(TEXT("Rollback preserves active value root"), Heap.IsAlive(OldObject));
	TestFalse(TEXT("Rollback releases candidate value root"), Heap.IsAlive(CandidateObject));
	FAvidScriptTaskResultSnapshot Snapshot;
	TestFalse(TEXT("Rollback invalidates candidate identity"), Rejected.ReadTaskResult(Candidate, Snapshot));
	auto& Published = Owner->BeginPrepared(World.Get());
	const int64 Next = Published.CreateRootedTaskResult(TEXT("test:Reference"));
	Lease = MakeLease(*this, Heap, NewObject);
	TestTrue(TEXT("Next candidate result publishes"), Published.SucceedRootedTaskResult(Next, Bytes(NewObject), MoveTemp(Lease), Woken));
	FString Error;
	TestTrue(TEXT("Rooted candidate passes current commit validation"), Owner->ValidatePreparedCommit(Error));
	Owner->CommitPrepared();
	TestTrue(TEXT("Commit collection succeeds"), Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Commit retires old active value root"), Heap.IsAlive(OldObject));
	TestTrue(TEXT("Commit preserves promoted value root"), Heap.IsAlive(NewObject));
	TestFalse(TEXT("Old endpoint rejects reads after commit"), Active.ReadTaskResult(Old, Snapshot));
	TestTrue(TEXT("Promoted result remains readable"), Published.ReadTaskResult(Next, Snapshot));
	Owner->Teardown();
	Owner->Teardown();
	TestTrue(TEXT("Teardown collection succeeds"), Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Teardown releases promoted value root"), Heap.IsAlive(NewObject));
	TestFalse(TEXT("Teardown rejects late reads"), Published.ReadTaskResult(Next, Snapshot));
	TestEqual(TEXT("Retirement leaves no task records"), Owner->GetTaskResultsForTesting().GetCount(), 0);
	TestEqual(TEXT("Retirement leaves no roots"), Heap.GetStats().LiveRoots, 0u);
	return true;
}

#endif
