#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"

#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include <array>

namespace AvidScriptTaskLanguageErrorTestPrivate
{
class FRootLease final : public IAvidScriptTaskLanguageErrorLease
{
public:
	explicit FRootLease(AvidScript::Managed::FPersistentRoots&& InRoots)
		: Roots(MoveTemp(InRoots)) {}
private:
	AvidScript::Managed::FPersistentRoots Roots;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSessionTaskResultsTest,
	"AvidScript.Runtime.Continuation.TaskResults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSessionTaskResultsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	FAvidScriptContinuationHostEndpoint& Host = Owner->ResetActive(nullptr);
	const uint64 ActiveSerial = Host.GetActivationSerial();
	FAvidScriptSessionTaskResults& Tasks = Owner->GetTaskResultsForTesting();
	const int64 Task = Tasks.Create(
		EAvidScriptContinuationLane::Active, ActiveSerial, TEXT("System.Int32"));
	TestNotEqual(TEXT("Task has an opaque nonzero identity"), Task, 0LL);
	TestTrue(TEXT("Task token uses a separate kind from continuation tokens"),
		(static_cast<uint64>(Task) & 0xc000000000000000ull)
			== 0x4000000000000000ull);
	TestFalse(TEXT("Continuation cancellation rejects a task token"), Host.Cancel(Task));
	TestFalse(TEXT("Cancellation source release rejects a task token"),
		Host.ReleaseCancellationSource(Task));
	TestTrue(TEXT("Second reference can retain the same task"), Tasks.Retain(Task));
	TestTrue(TEXT("First waiter queues"), Tasks.RegisterWaiter(Task, 11)
		== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Second waiter queues"), Tasks.RegisterWaiter(Task, 22)
		== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Duplicate waiter is rejected"), Tasks.RegisterWaiter(Task, 11)
		== EAvidScriptTaskWaitRegistration::Invalid);
	TestTrue(TEXT("Pending waiter may unregister"), Tasks.UnregisterWaiter(Task, 22));
	TestFalse(TEXT("Unregistered waiter cannot unregister twice"),
		Tasks.UnregisterWaiter(Task, 22));
	TestTrue(TEXT("Removed waiter can queue again"), Tasks.RegisterWaiter(Task, 22)
		== EAvidScriptTaskWaitRegistration::Queued);
	TestEqual(TEXT("Two pending waiters are retained"), Tasks.GetWaiterCount(), 2);

	const int32 Result = 12;
	const TConstArrayView<uint8> ResultBytes(
		reinterpret_cast<const uint8*>(&Result), sizeof(Result));
	TArray<int64> Woken;
	TestTrue(TEXT("Task completes once"), Tasks.Succeed(Task, ResultBytes, Woken));
	TestEqual(TEXT("Two waiters wake"), Woken.Num(), 2);
	TestEqual(TEXT("First waiter keeps registration order"), Woken[0], 11LL);
	TestEqual(TEXT("Second waiter keeps registration order"), Woken[1], 22LL);
	TestEqual(TEXT("Completion clears pending waiter storage"), Tasks.GetWaiterCount(), 0);
	TestFalse(TEXT("Duplicate completion is rejected"), Tasks.Succeed(Task, ResultBytes, Woken));
	TestTrue(TEXT("Late waiter sees ready state"), Tasks.RegisterWaiter(Task, 33)
		== EAvidScriptTaskWaitRegistration::Ready);

	FAvidScriptTaskResultSnapshot First;
	FAvidScriptTaskResultSnapshot Second;
	TestTrue(TEXT("First reader sees result"), Tasks.Read(Task, First));
	TestTrue(TEXT("Second reader sees result"), Tasks.Read(Task, Second));
	TestTrue(TEXT("Result is successful"), First.State == EAvidScriptTaskResultState::Succeeded);
	TestEqual(TEXT("Result type is stable"), First.TypeId, FString(TEXT("System.Int32")));
	TestEqual(TEXT("Result bytes remain available"), First.Value.Num(),
		static_cast<int32>(sizeof(Result)));
	TestTrue(TEXT("Both readers see identical bytes"), First.Value == Second.Value);
	TestTrue(TEXT("First reference releases"), Tasks.Release(Task));
	TestTrue(TEXT("Second reference releases"), Tasks.Release(Task));
	TestEqual(TEXT("Last release reclaims result"), Tasks.GetCount(), 0);
	TestFalse(TEXT("Released task token is stale"), Tasks.Read(Task, First));
	const int64 Unowned = Tasks.Create(
		EAvidScriptContinuationLane::Active, ActiveSerial, TEXT("System.Int32"));
	TestTrue(TEXT("Running task may release its last handle"), Tasks.Release(Unowned));
	TestFalse(TEXT("Zero-reference running task cannot be resurrected"),
		Tasks.Retain(Unowned));
	TestTrue(TEXT("Unowned producer completion reclaims the task"),
		Tasks.Succeed(Unowned, ResultBytes, Woken));
	TestFalse(TEXT("Reclaimed task token is stale"), Tasks.Read(Unowned, First));

	const int64 Faulted = Tasks.Create(
		EAvidScriptContinuationLane::Active, ActiveSerial, TEXT("System.Int32"));
	TestTrue(TEXT("Language failure records a terminal state"),
		Tasks.Fault(Faulted, TEXT("script_error"), Woken));
	TestTrue(TEXT("Fault remains readable"), Tasks.Read(Faulted, First));
	TestTrue(TEXT("Fault state is distinct"), First.State == EAvidScriptTaskResultState::Faulted);
	TestEqual(TEXT("Fault code is retained"), First.ErrorCode, FString(TEXT("script_error")));
	TestFalse(TEXT("Fault cannot be overwritten"), Tasks.Cancel(Faulted, Woken));
	TestTrue(TEXT("Fault reference releases"), Tasks.Release(Faulted));
	const int64 Cancelled = Tasks.Create(
		EAvidScriptContinuationLane::Active, ActiveSerial, TEXT("System.Int32"));
	TestTrue(TEXT("Explicit cancellation reaches a terminal state"),
		Tasks.Cancel(Cancelled, Woken));
	TestTrue(TEXT("Cancellation remains readable"), Tasks.Read(Cancelled, First));
	TestTrue(TEXT("Cancellation has its own state"),
		First.State == EAvidScriptTaskResultState::Cancelled);
	TestTrue(TEXT("Cancellation reference releases"), Tasks.Release(Cancelled));

	const int64 ActiveTask = Tasks.Create(
		EAvidScriptContinuationLane::Active, ActiveSerial, TEXT("System.Int32"));
	const uint64 RejectedSerial = Owner->BeginPrepared(nullptr).GetActivationSerial();
	const int64 RejectedTask = Tasks.Create(
		EAvidScriptContinuationLane::Prepared, RejectedSerial, TEXT("System.Int32"));
	Owner->DiscardPrepared();
	TestFalse(TEXT("Rollback retires only candidate task"), Tasks.Retain(RejectedTask));
	TestTrue(TEXT("Rollback preserves active task"), Tasks.Retain(ActiveTask));
	TestTrue(TEXT("Retained active task releases"), Tasks.Release(ActiveTask));

	const uint64 PublishedSerial = Owner->BeginPrepared(nullptr).GetActivationSerial();
	const int64 PublishedTask = Tasks.Create(
		EAvidScriptContinuationLane::Prepared, PublishedSerial, TEXT("System.Int32"));
	Owner->CommitPrepared();
	TestFalse(TEXT("Publish retires old active task"), Tasks.Retain(ActiveTask));
	TestTrue(TEXT("Publish promotes candidate task"), Tasks.Retain(PublishedTask));
	TestTrue(TEXT("Promoted task releases extra reference"), Tasks.Release(PublishedTask));
	Owner->Teardown();
	TestEqual(TEXT("Teardown reclaims all tasks"), Tasks.GetCount(), 0);
	TestFalse(TEXT("Teardown invalidates published task"), Tasks.Retain(PublishedTask));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSessionTaskLanguageErrorTest,
	"AvidScript.Runtime.Continuation.TaskLanguageError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSessionTaskLanguageErrorTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	FHeap Heap;
	const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
	if (!TestTrue(TEXT("Error heap configures"), Heap.Configure(Layouts) == EHeapError::Ok))
	{
		return false;
	}
	FToken Frame = 0, FrameRoot = 0, ErrorObject = 0;
	TestTrue(TEXT("Error invocation frame starts"), Heap.PushFrame(Frame) == EHeapError::Ok);
	TestTrue(TEXT("Error frame root exists"), Heap.CreateRoot(Frame, 0, FrameRoot) == EHeapError::Ok);
	TestTrue(TEXT("Error object allocates in frame"), Heap.Allocate(1, FrameRoot, ErrorObject) == EHeapError::Ok);
	TestTrue(TEXT("Object has caller root authority"),
		Heap.IsObjectRootedInCurrentFrame(ErrorObject));
	const std::array<FToken, 1> Objects{{ErrorObject}};
	FPersistentRoots Roots;
	TestTrue(TEXT("Task acquires a persistent root"),
		Heap.RetainPersistent(Objects, Roots) == EHeapError::Ok);
	TestTrue(TEXT("Invocation frame exits"), Heap.PopFrame(Frame) == EHeapError::Ok);
	TSharedPtr<IAvidScriptTaskLanguageErrorLease> Lease =
		MakeShared<AvidScriptTaskLanguageErrorTestPrivate::FRootLease>(MoveTemp(Roots));

	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	FAvidScriptContinuationHostEndpoint& Host = Owner->ResetActive(World.Get());
	const int64 Source = Host.CreateTaskResult(TEXT("type:int32"));
	const int64 Target = Host.CreateTaskResult(TEXT("type:int32"));
	if (!TestNotEqual(TEXT("Source task exists"), Source, 0LL)
		|| !TestNotEqual(TEXT("Target task exists"), Target, 0LL))
	{
		return false;
	}
	int64 Waiter = 0;
	TestTrue(TEXT("Target waiter registers"),
		Host.AwaitTaskResult(Target, 301, Waiter)
			== EAvidScriptTaskWaitRegistration::Queued);
	TArray<int64> Woken;
	FAvidScriptTaskLanguageError Error{3, 7, ErrorObject};
	TestTrue(TEXT("Language error faults source"), Host.FaultTaskResultLanguageError(
		Source, Error, MoveTemp(Lease), Woken));
	TestTrue(TEXT("Caller releases root lease ownership"), !Lease.IsValid());
	TestTrue(TEXT("Root survives after frame exit"), Heap.Collect() == EHeapError::Ok);
	TestTrue(TEXT("Faulted source keeps error object"), Heap.IsAlive(ErrorObject));
	TestTrue(TEXT("Fault propagates to target"),
		Host.PropagateTaskFailure(Source, Target, Woken));
	TestEqual(TEXT("Target waiter wakes once"), Woken.Num(), 1);
	if (Woken.Num() == 1)
	{
		TestEqual(TEXT("Correct waiter wakes"), Woken[0], Waiter);
	}
	FAvidScriptTaskResultSnapshot Snapshot;
	TestTrue(TEXT("Target result remains readable"), Host.ReadTaskResult(Target, Snapshot));
	TestTrue(TEXT("Target result is faulted"),
		Snapshot.State == EAvidScriptTaskResultState::Faulted);
	TestEqual(TEXT("Fault code survives propagation"),
		Snapshot.ErrorCode, FString(TEXT("language_error")));
	TestTrue(TEXT("Language payload survives propagation"), Snapshot.LanguageError.IsSet());
	if (Snapshot.LanguageError.IsSet())
	{
		TestEqual(TEXT("Type token survives"), Snapshot.LanguageError->TypeToken, 3);
		TestEqual(TEXT("Source token survives"), Snapshot.LanguageError->SourceToken, 7);
		TestEqual(TEXT("Object token survives"), Snapshot.LanguageError->ObjectToken, ErrorObject);
	}
	TestFalse(TEXT("Fault cannot propagate twice to completed target"),
		Host.PropagateTaskFailure(Source, Target, Woken));
	TestTrue(TEXT("Source reference releases"), Host.ReleaseTaskResult(Source));
	TestTrue(TEXT("Target root survives source retirement"), Heap.Collect() == EHeapError::Ok);
	TestTrue(TEXT("Target still owns error object"), Heap.IsAlive(ErrorObject));
	TestTrue(TEXT("Producer releases target reference"), Host.ReleaseTaskResult(Target));
	TArray<FAvidScriptContinuationCompletion> Ready;
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Faulted target dispatches waiter"), Ready.Num(), 1);
	TestTrue(TEXT("Waiter finalizes"), Owner->FinalizeDispatched(Waiter, true));
	TestTrue(TEXT("Collected object is released with last waiter"),
		Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("No error object remains after task release"), Heap.IsAlive(ErrorObject));
	TestEqual(TEXT("No persistent error roots remain"), Heap.GetStats().LiveRoots, 0u);

	FToken CandidateFrame = 0, CandidateFrameRoot = 0, CandidateObject = 0;
	TestTrue(TEXT("Candidate frame starts"),
		Heap.PushFrame(CandidateFrame) == EHeapError::Ok);
	TestTrue(TEXT("Candidate frame root exists"),
		Heap.CreateRoot(CandidateFrame, 0, CandidateFrameRoot) == EHeapError::Ok);
	TestTrue(TEXT("Candidate error object allocates"),
		Heap.Allocate(1, CandidateFrameRoot, CandidateObject) == EHeapError::Ok);
	const std::array<FToken, 1> CandidateObjects{{CandidateObject}};
	FPersistentRoots CandidateRoots;
	TestTrue(TEXT("Candidate task acquires root"),
		Heap.RetainPersistent(CandidateObjects, CandidateRoots) == EHeapError::Ok);
	TestTrue(TEXT("Candidate frame exits"),
		Heap.PopFrame(CandidateFrame) == EHeapError::Ok);
	FAvidScriptContinuationHostEndpoint& Prepared = Owner->BeginPrepared(World.Get());
	const int64 Candidate = Prepared.CreateTaskResult(TEXT("type:int32"));
	TSharedPtr<IAvidScriptTaskLanguageErrorLease> CandidateLease =
		MakeShared<AvidScriptTaskLanguageErrorTestPrivate::FRootLease>(
			MoveTemp(CandidateRoots));
	TestTrue(TEXT("Prepared task stores its error root"),
		Prepared.FaultTaskResultLanguageError(Candidate,
			{5, 9, CandidateObject}, MoveTemp(CandidateLease), Woken));
	Owner->DiscardPrepared();
	TestTrue(TEXT("Rollback collects discarded error object"),
		Heap.Collect() == EHeapError::Ok);
	TestFalse(TEXT("Candidate root does not survive rollback"),
		Heap.IsAlive(CandidateObject));
	TestFalse(TEXT("Rolled-back task token is stale"),
		Prepared.ReadTaskResult(Candidate, Snapshot));
	Owner->Teardown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSessionTaskEndpointTest,
	"AvidScript.Runtime.Continuation.TaskEndpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSessionTaskEndpointTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	FAvidScriptContinuationHostEndpoint& MissingWorld = Owner->ResetActive(nullptr);
	TestEqual(TEXT("A task cannot be created without a live world"),
		MissingWorld.CreateTaskResult(TEXT("System.Int32")), 0LL);

	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	FAvidScriptContinuationHostEndpoint& Active = Owner->ResetActive(World.Get());
	const int64 ActiveTask = Active.CreateTaskResult(TEXT("System.Int32"));
	TestNotEqual(TEXT("Active endpoint creates a Session-owned task"), ActiveTask, 0LL);
	TestEqual(TEXT("Empty task type is rejected"), Active.CreateTaskResult({}), 0LL);
	TestTrue(TEXT("Active endpoint retains its own task"), Active.RetainTaskResult(ActiveTask));
	int64 ActiveWaiter = 0;
	TestTrue(TEXT("Active waiter queues"),
		Active.AwaitTaskResult(ActiveTask, 101, ActiveWaiter)
			== EAvidScriptTaskWaitRegistration::Queued);
	TestNotEqual(TEXT("Waiter receives a continuation token"), ActiveWaiter, 0LL);
	TestEqual(TEXT("Session retains the pending waiter"), Owner->GetActiveCount(), 1);

	FAvidScriptContinuationHostEndpoint& Prepared = Owner->BeginPrepared(World.Get());
	const int64 PreparedTask = Prepared.CreateTaskResult(TEXT("System.Int32"));
	TestNotEqual(TEXT("Prepared endpoint creates a separate task"), PreparedTask, 0LL);
	FAvidScriptTaskResultSnapshot Snapshot;
	TArray<int64> Woken;
	const int32 Value = 12;
	const TConstArrayView<uint8> ValueBytes(
		reinterpret_cast<const uint8*>(&Value), sizeof(Value));
	TestFalse(TEXT("Prepared endpoint cannot read an active task"),
		Prepared.ReadTaskResult(ActiveTask, Snapshot));
	TestFalse(TEXT("Prepared endpoint cannot complete an active task"),
		Prepared.SucceedTaskResult(ActiveTask, ValueBytes, Woken));
	TestFalse(TEXT("Active endpoint cannot cancel a prepared task"),
		Active.CancelTaskResult(PreparedTask, Woken));
	TestTrue(TEXT("Active task completes"),
		Active.SucceedTaskResult(ActiveTask, ValueBytes, Woken));
	TestEqual(TEXT("Completion returns its waiter"), Woken.Num(), 1);
	TestEqual(TEXT("Waiter identity is preserved"), Woken[0], ActiveWaiter);
	TArray<FAvidScriptContinuationCompletion> Ready;
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Completion enters dispatcher once"), Ready.Num(), 1);
	if (Ready.Num() == 1)
	{
		TestEqual(TEXT("Task callback identity is preserved"), Ready[0].CallbackId, 101);
		TestEqual(TEXT("Task continuation token is preserved"),
			Ready[0].Token, ActiveWaiter);
		TestTrue(TEXT("Success resumes with completed status"),
			Ready[0].Status == EAvidScriptContinuationStatus::Completed);
	}
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Task completion does not dispatch twice"), Ready.Num(), 0);
	TestTrue(TEXT("Active result is repeatable"), Active.ReadTaskResult(ActiveTask, Snapshot));
	TestTrue(TEXT("Active result is successful"),
		Snapshot.State == EAvidScriptTaskResultState::Succeeded);
	TestTrue(TEXT("Dispatched waiter releases its task reference"),
		Owner->FinalizeDispatched(ActiveWaiter, true));
	TestEqual(TEXT("Dispatcher releases the waiter slot"), Owner->GetActiveCount(), 0);
	int64 AlreadyReady = -1;
	TestTrue(TEXT("Late await observes the terminal task"),
		Active.AwaitTaskResult(ActiveTask, 102, AlreadyReady)
			== EAvidScriptTaskWaitRegistration::Ready);
	TestEqual(TEXT("Ready await needs no continuation"), AlreadyReady, 0LL);
	TestTrue(TEXT("First active reference releases"), Active.ReleaseTaskResult(ActiveTask));
	TestTrue(TEXT("Second active reference releases"), Active.ReleaseTaskResult(ActiveTask));
	TestFalse(TEXT("Released task cannot be read"),
		Active.ReadTaskResult(ActiveTask, Snapshot));

	FString CommitError;
	TestTrue(TEXT("Live prepared task allows publication"),
		Owner->ValidatePreparedCommit(CommitError));
	Owner->CommitPrepared();
	TestEqual(TEXT("Old endpoint rejects creation after publication"),
		Active.CreateTaskResult(TEXT("System.Int32")), 0LL);
	TestTrue(TEXT("Promoted endpoint retains its task identity"),
		Prepared.RetainTaskResult(PreparedTask));
	TestTrue(TEXT("Promoted task can complete"),
		Prepared.SucceedTaskResult(PreparedTask, ValueBytes, Woken));
	TestTrue(TEXT("Promoted task remains readable"),
		Prepared.ReadTaskResult(PreparedTask, Snapshot));
	Owner->Teardown();
	TestFalse(TEXT("Teardown rejects late task reads"),
		Prepared.ReadTaskResult(PreparedTask, Snapshot));
	TestEqual(TEXT("Teardown retires task storage"),
		Owner->GetTaskResultsForTesting().GetCount(), 0);

	FAvidScriptContinuationHostEndpoint& InvalidPrepared = Owner->BeginPrepared(nullptr);
	const int64 Orphan = Owner->GetTaskResultsForTesting().Create(
		EAvidScriptContinuationLane::Prepared,
		InvalidPrepared.GetActivationSerial(), TEXT("System.Int32"));
	TestNotEqual(TEXT("Test fixture seeds a candidate task"), Orphan, 0LL);
	TestFalse(TEXT("Candidate task with no live context cannot publish"),
		Owner->ValidatePreparedCommit(CommitError));
	Owner->DiscardPrepared();
	TestEqual(TEXT("Rollback retires orphan candidate task"),
		Owner->GetTaskResultsForTesting().GetCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSessionTaskDispatchTest,
	"AvidScript.Runtime.Continuation.TaskDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSessionTaskDispatchTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	FAvidScriptContinuationHostEndpoint& Host = Owner->ResetActive(World.Get());
	FAvidScriptSessionTaskResults& Tasks = Owner->GetTaskResultsForTesting();
	const int64 Task = Host.CreateTaskResult(TEXT("System.Int32"));
	TestNotEqual(TEXT("Multiwaiter task is created"), Task, 0LL);
	int64 First = 0;
	int64 Removed = 0;
	int64 Last = 0;
	TestTrue(TEXT("First waiter queues"), Host.AwaitTaskResult(Task, 201, First)
		== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Removable waiter queues"), Host.AwaitTaskResult(Task, 202, Removed)
		== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Last waiter queues"), Host.AwaitTaskResult(Task, 203, Last)
		== EAvidScriptTaskWaitRegistration::Queued);
	TestEqual(TEXT("Three waiters are registered"), Tasks.GetWaiterCount(), 3);
	TestTrue(TEXT("Explicit cancellation releases one waiter"), Host.Cancel(Removed));
	TestEqual(TEXT("Cancellation unregisters the waiter"), Tasks.GetWaiterCount(), 2);
	int64 InvalidWaiter = -1;
	TestTrue(TEXT("Invalid callback is rejected"),
		Host.AwaitTaskResult(Task, 0, InvalidWaiter)
			== EAvidScriptTaskWaitRegistration::Invalid);
	TestEqual(TEXT("Rejected await has no token"), InvalidWaiter, 0LL);

	const int32 Value = 34;
	const TConstArrayView<uint8> ValueBytes(
		reinterpret_cast<const uint8*>(&Value), sizeof(Value));
	TArray<int64> Woken;
	TestTrue(TEXT("Task completion queues remaining waiters"),
		Host.SucceedTaskResult(Task, ValueBytes, Woken));
	TestEqual(TEXT("Only live waiters wake"), Woken.Num(), 2);
	if (Woken.Num() == 2)
	{
		TestEqual(TEXT("First waiter wakes first"), Woken[0], First);
		TestEqual(TEXT("Last waiter wakes second"), Woken[1], Last);
	}
	TestTrue(TEXT("Producer may release its result before dispatch"),
		Host.ReleaseTaskResult(Task));
	TArray<FAvidScriptContinuationCompletion> Ready;
	Owner->DrainReady(Ready);
	TestEqual(TEXT("First registered callback dispatches first"), Ready.Num(), 1);
	if (Ready.Num() == 1)
	{
		TestEqual(TEXT("First callback ID"), Ready[0].CallbackId, 201);
	}
	FAvidScriptTaskResultSnapshot Snapshot;
	TestTrue(TEXT("First waiter can still read terminal result"),
		Host.ReadTaskResult(Task, Snapshot));
	TestTrue(TEXT("First waiter finalizes"), Owner->FinalizeDispatched(First, true));
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Second registered callback dispatches second"), Ready.Num(), 1);
	if (Ready.Num() == 1)
	{
		TestEqual(TEXT("Second callback ID"), Ready[0].CallbackId, 203);
	}
	TestTrue(TEXT("Second waiter can still read terminal result"),
		Host.ReadTaskResult(Task, Snapshot));
	TestTrue(TEXT("Second waiter finalizes"), Owner->FinalizeDispatched(Last, true));
	TestEqual(TEXT("Last waiter releases task result"), Tasks.GetCount(), 0);
	Owner->DrainReady(Ready);
	TestEqual(TEXT("No duplicate completion remains"), Ready.Num(), 0);

	const int64 Faulted = Host.CreateTaskResult(TEXT("System.Int32"));
	int64 FaultWaiter = 0;
	TestTrue(TEXT("Fault waiter queues"), Host.AwaitTaskResult(Faulted, 204, FaultWaiter)
		== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Task fault is accepted"),
		Host.FaultTaskResult(Faulted, TEXT("script_error"), Woken));
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Fault dispatches once"), Ready.Num(), 1);
	if (Ready.Num() == 1)
	{
		TestTrue(TEXT("Fault maps to failed completion"),
			Ready[0].Status == EAvidScriptContinuationStatus::Failed);
	}
	TestTrue(TEXT("Faulted task remains readable during dispatch"),
		Host.ReadTaskResult(Faulted, Snapshot));
	TestEqual(TEXT("Fault code is preserved"), Snapshot.ErrorCode,
		FString(TEXT("script_error")));
	TestTrue(TEXT("Fault producer releases"), Host.ReleaseTaskResult(Faulted));
	TestTrue(TEXT("Fault waiter finalizes"), Owner->FinalizeDispatched(FaultWaiter, true));

	const int64 Cancelled = Host.CreateTaskResult(TEXT("System.Int32"));
	int64 CancelWaiter = 0;
	TestTrue(TEXT("Cancel waiter queues"), Host.AwaitTaskResult(Cancelled, 205, CancelWaiter)
		== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Task cancellation is accepted"),
		Host.CancelTaskResult(Cancelled, Woken));
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Cancellation dispatches once"), Ready.Num(), 1);
	if (Ready.Num() == 1)
	{
		TestTrue(TEXT("Task cancellation maps to cancelled completion"),
			Ready[0].Status == EAvidScriptContinuationStatus::Cancelled);
	}
	TestTrue(TEXT("Cancelled task remains readable during dispatch"),
		Host.ReadTaskResult(Cancelled, Snapshot));
	TestTrue(TEXT("Cancel producer releases"), Host.ReleaseTaskResult(Cancelled));
	TestTrue(TEXT("Cancel waiter finalizes"), Owner->FinalizeDispatched(CancelWaiter, true));

	const int64 Pending = Host.CreateTaskResult(TEXT("System.Int32"));
	int64 PendingWaiter = 0;
	TestTrue(TEXT("Teardown waiter queues"), Host.AwaitTaskResult(Pending, 206, PendingWaiter)
		== EAvidScriptTaskWaitRegistration::Queued);
	Owner->Teardown();
	TestEqual(TEXT("Teardown retires every task"), Tasks.GetCount(), 0);
	TestEqual(TEXT("Teardown retires every waiter"), Tasks.GetWaiterCount(), 0);
	TestEqual(TEXT("Teardown retires every continuation"), Owner->GetActiveCount(), 0);
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Teardown cannot resume a waiter"), Ready.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSessionTaskProducerBindingTest,
	"AvidScript.Runtime.Continuation.TaskProducerBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSessionTaskProducerBindingTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	FAvidScriptContinuationHostEndpoint& Host = Owner->ResetActive(World.Get());
	FAvidScriptSessionTaskResults& Tasks = Owner->GetTaskResultsForTesting();
	FAvidScriptTaskResultSnapshot Snapshot;
	TArray<FAvidScriptContinuationCompletion> Ready;
	TArray<int64> Woken;
	const int32 Value = 7;
	const TConstArrayView<uint8> ValueBytes(
		reinterpret_cast<const uint8*>(&Value), sizeof(Value));

	const int64 CancelledTask = Host.CreateTaskResult(TEXT("System.Int32"));
	TestTrue(TEXT("Producer owns a second result reference"),
		Host.RetainTaskResult(CancelledTask));
	const int64 CancelledProducer = Host.ScheduleDelay(30.0f, 301);
	TestTrue(TEXT("Pending producer binds to its result"),
		Host.BindTaskProducer(CancelledTask, CancelledProducer));
	TestFalse(TEXT("Duplicate producer binding is rejected"),
		Host.BindTaskProducer(CancelledTask, CancelledProducer));
	TestTrue(TEXT("Producer transfers ownership to Session"),
		Host.ReleaseTaskResult(CancelledTask));
	int64 CancelledWaiter = 0;
	TestTrue(TEXT("Caller awaits producer result"),
		Host.AwaitTaskResult(CancelledTask, 302, CancelledWaiter)
			== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Caller releases its result reference"),
		Host.ReleaseTaskResult(CancelledTask));
	const int64 Source = Host.CreateCancellationSource();
	TestTrue(TEXT("Cancellation source binds producer"),
		Host.BindCancellationSource(Source, CancelledProducer));
	TestTrue(TEXT("Cancellation source cancels producer"),
		Host.CancelCancellationSource(Source));
	TestFalse(TEXT("Cancelled producer token is stale"), Host.Cancel(CancelledProducer));
	TestTrue(TEXT("Waiter can read cancelled result"),
		Host.ReadTaskResult(CancelledTask, Snapshot));
	TestTrue(TEXT("Cancellation propagates to task"),
		Snapshot.State == EAvidScriptTaskResultState::Cancelled);
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Cancellation wakes one waiter"), Ready.Num(), 1);
	if (Ready.Num() == 1)
	{
		TestEqual(TEXT("Cancelled waiter identity"), Ready[0].Token, CancelledWaiter);
		TestTrue(TEXT("Waiter receives cancelled status"),
			Ready[0].Status == EAvidScriptContinuationStatus::Cancelled);
	}
	TestTrue(TEXT("Cancelled waiter finalizes"),
		Owner->FinalizeDispatched(CancelledWaiter, true));
	TestEqual(TEXT("Cancelled task references are reclaimed"), Tasks.GetCount(), 0);
	TestTrue(TEXT("Cancellation source releases"),
		Host.ReleaseCancellationSource(Source));

	const int64 Trigger = Host.CreateTaskResult(TEXT("System.Int32"));
	const int64 TransferredTask = Host.CreateTaskResult(TEXT("System.Int32"));
	int64 FirstProducer = 0;
	TestTrue(TEXT("Producer awaits trigger"),
		Host.AwaitTaskResult(Trigger, 303, FirstProducer)
			== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("First producer binding succeeds"),
		Host.BindTaskProducer(TransferredTask, FirstProducer));
	TestFalse(TEXT("Parallel producer binding is rejected"),
		Host.BindTaskProducer(TransferredTask, Host.ScheduleDelay(30.0f, 304)));
	TestTrue(TEXT("Trigger completes"), Host.SucceedTaskResult(Trigger, ValueBytes, Woken));
	TestTrue(TEXT("Trigger caller releases"), Host.ReleaseTaskResult(Trigger));
	Owner->DrainReady(Ready);
	TestEqual(TEXT("First producer dispatches"), Ready.Num(), 1);
	const int64 NextProducer = Host.ScheduleDelay(30.0f, 305);
	TestTrue(TEXT("Dispatch transfers producer binding"),
		Host.BindTaskProducer(TransferredTask, NextProducer));
	TestTrue(TEXT("First producer finalizes without ending task"),
		Owner->FinalizeDispatched(FirstProducer, true));
	TestTrue(TEXT("Task remains pending after transfer"),
		!Host.ReadTaskResult(TransferredTask, Snapshot));
	TestTrue(TEXT("New producer cancels"), Host.Cancel(NextProducer));
	TestTrue(TEXT("Transferred cancellation reaches task"),
		Host.ReadTaskResult(TransferredTask, Snapshot));
	TestTrue(TEXT("Transferred task is cancelled"),
		Snapshot.State == EAvidScriptTaskResultState::Cancelled);
	TestTrue(TEXT("Caller releases transferred task"),
		Host.ReleaseTaskResult(TransferredTask));

	const int64 FaultTrigger = Host.CreateTaskResult(TEXT("System.Int32"));
	const int64 FaultTask = Host.CreateTaskResult(TEXT("System.Int32"));
	int64 FaultProducer = 0;
	TestTrue(TEXT("Faulting producer awaits trigger"),
		Host.AwaitTaskResult(FaultTrigger, 306, FaultProducer)
			== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Faulting producer binds result"),
		Host.BindTaskProducer(FaultTask, FaultProducer));
	TestTrue(TEXT("Fault trigger completes"),
		Host.SucceedTaskResult(FaultTrigger, ValueBytes, Woken));
	TestTrue(TEXT("Fault trigger caller releases"),
		Host.ReleaseTaskResult(FaultTrigger));
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Faulting producer dispatches"), Ready.Num(), 1);
	TestTrue(TEXT("Failed producer finalizes"),
		Owner->FinalizeDispatched(FaultProducer, false));
	TestTrue(TEXT("Failed dispatch ends result"),
		Host.ReadTaskResult(FaultTask, Snapshot));
	TestTrue(TEXT("Failed dispatch faults task"),
		Snapshot.State == EAvidScriptTaskResultState::Faulted);
	TestEqual(TEXT("Failed dispatch stores stable error code"),
		Snapshot.ErrorCode, FString(TEXT("script_execution_failed")));
	TestTrue(TEXT("Faulted result caller releases"),
		Host.ReleaseTaskResult(FaultTask));
	FAvidScriptContinuationHostEndpoint& Prepared = Owner->BeginPrepared(World.Get());
	const int64 PreparedTask = Prepared.CreateTaskResult(TEXT("System.Int32"));
	const int64 ActiveContinuation = Host.ScheduleDelay(30.0f, 307);
	TestFalse(TEXT("Active continuation rejects prepared result"),
		Host.BindTaskProducer(PreparedTask, ActiveContinuation));
	TestFalse(TEXT("Prepared endpoint rejects active continuation"),
		Prepared.BindTaskProducer(PreparedTask, ActiveContinuation));
	TestFalse(TEXT("Released task token cannot be rebound"),
		Host.BindTaskProducer(FaultTask, ActiveContinuation));
	TestTrue(TEXT("Unused active continuation cancels"),
		Host.Cancel(ActiveContinuation));
	Owner->DiscardPrepared();
	Owner->Teardown();
	TestEqual(TEXT("Teardown retires remaining continuations"),
		Owner->GetActiveCount(), 0);
	TestEqual(TEXT("Teardown retires all task results"), Tasks.GetCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSessionTaskContinuationOwnershipTest,
	"AvidScript.Runtime.Continuation.TaskContinuationOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSessionTaskContinuationOwnershipTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	FAvidScriptContinuationHostEndpoint& Host = Owner->ResetActive(World.Get());
	FAvidScriptSessionTaskResults& Tasks = Owner->GetTaskResultsForTesting();
	const TArray<uint8> StateBytes{1};
	const int32 Value = 19;
	const TConstArrayView<uint8> ValueBytes(
		reinterpret_cast<const uint8*>(&Value), sizeof(Value));
	TArray<int64> Woken;
	TArray<FAvidScriptContinuationCompletion> Ready;
	FAvidScriptTaskResultSnapshot Snapshot;

	const int64 CancelledTask = Host.CreateTaskResult(TEXT("System.Int32"));
	const int64 CancelledContinuation = Host.ScheduleDelay(30.0f, 401);
	TestFalse(TEXT("Task reference requires a stored continuation frame"),
		Host.RetainTaskForContinuation(CancelledTask, CancelledContinuation));
	TestTrue(TEXT("Cancellation continuation stores its frame"),
		Host.StoreState(CancelledContinuation, StateBytes));
	TestTrue(TEXT("Continuation retains the local task"),
		Host.RetainTaskForContinuation(CancelledTask, CancelledContinuation));
	TestFalse(TEXT("A task token is not a continuation"),
		Host.RetainTaskForContinuation(CancelledTask, CancelledTask));
	TestTrue(TEXT("Caller transfers its reference"),
		Host.ReleaseTaskResult(CancelledTask));
	TestTrue(TEXT("Captured task can finish without its caller"),
		Host.SucceedTaskResult(CancelledTask, ValueBytes, Woken));
	TestTrue(TEXT("Cancellation retires the capture"), Host.Cancel(CancelledContinuation));
	TestEqual(TEXT("Cancelled continuation releases its task"), Tasks.GetCount(), 0);
	TestFalse(TEXT("Retired continuation cannot capture another task"),
		Host.RetainTaskForContinuation(CancelledTask, CancelledContinuation));

	const int64 Trigger = Host.CreateTaskResult(TEXT("System.Int32"));
	const int64 ResumedTask = Host.CreateTaskResult(TEXT("System.Int32"));
	int64 Waiter = 0;
	TestTrue(TEXT("A task result schedules the resuming continuation"),
		Host.AwaitTaskResult(Trigger, 402, Waiter)
			== EAvidScriptTaskWaitRegistration::Queued);
	TestTrue(TEXT("Resuming continuation stores its frame"),
		Host.StoreState(Waiter, StateBytes));
	TestTrue(TEXT("Resuming continuation retains another local task"),
		Host.RetainTaskForContinuation(ResumedTask, Waiter));
	TestTrue(TEXT("Caller releases the local task before suspension"),
		Host.ReleaseTaskResult(ResumedTask));
	TestTrue(TEXT("Local task completes while continuation is pending"),
		Host.SucceedTaskResult(ResumedTask, ValueBytes, Woken));
	TestTrue(TEXT("Trigger completes"), Host.SucceedTaskResult(Trigger, ValueBytes, Woken));
	TestTrue(TEXT("Trigger caller releases"), Host.ReleaseTaskResult(Trigger));
	Owner->DrainReady(Ready);
	TestEqual(TEXT("Resuming continuation dispatches once"), Ready.Num(), 1);
	TestTrue(TEXT("Guest reclaims the task during dispatch"),
		Host.RetainTaskResult(ResumedTask));
	TestTrue(TEXT("Dispatch finalizes"), Owner->FinalizeDispatched(Waiter, true));
	TestEqual(TEXT("Only the reclaimed task remains"), Tasks.GetCount(), 1);
	TestTrue(TEXT("Reclaimed task result remains readable"),
		Host.ReadTaskResult(ResumedTask, Snapshot));
	TestEqual(TEXT("Reclaimed result has an int payload"),
		Snapshot.Value.Num(), static_cast<int32>(sizeof(Value)));
	if (Snapshot.Value.Num() == sizeof(Value))
	{
		int32 ReadValue = 0;
		FMemory::Memcpy(&ReadValue, Snapshot.Value.GetData(), sizeof(ReadValue));
		TestEqual(TEXT("Reclaimed task result keeps its value"), ReadValue, Value);
	}
	TestTrue(TEXT("Guest releases its final task reference"),
		Host.ReleaseTaskResult(ResumedTask));
	TestEqual(TEXT("Completed path releases every task"), Tasks.GetCount(), 0);

	const int64 ActiveTask = Host.CreateTaskResult(TEXT("System.Int32"));
	const int64 ActiveContinuation = Host.ScheduleDelay(30.0f, 403);
	TestTrue(TEXT("Active continuation stores its frame"),
		Host.StoreState(ActiveContinuation, StateBytes));
	FAvidScriptContinuationHostEndpoint& Prepared = Owner->BeginPrepared(World.Get());
	const int64 PreparedTask = Prepared.CreateTaskResult(TEXT("System.Int32"));
	TestFalse(TEXT("Active endpoint rejects a prepared task"),
		Host.RetainTaskForContinuation(PreparedTask, ActiveContinuation));
	TestFalse(TEXT("Prepared endpoint rejects an active continuation"),
		Prepared.RetainTaskForContinuation(PreparedTask, ActiveContinuation));
	TestTrue(TEXT("Active continuation still accepts its own task"),
		Host.RetainTaskForContinuation(ActiveTask, ActiveContinuation));
	TestTrue(TEXT("Active caller transfers task ownership"),
		Host.ReleaseTaskResult(ActiveTask));
	Owner->DiscardPrepared();
	TestFalse(TEXT("Discarded prepared task cannot be captured"),
		Host.RetainTaskForContinuation(PreparedTask, ActiveContinuation));
	TestTrue(TEXT("Active continuation cancels after rollback"),
		Host.Cancel(ActiveContinuation));

	const int64 TeardownTask = Host.CreateTaskResult(TEXT("System.Int32"));
	const int64 TeardownContinuation = Host.ScheduleDelay(30.0f, 404);
	TestTrue(TEXT("Teardown continuation stores its frame"),
		Host.StoreState(TeardownContinuation, StateBytes));
	TestTrue(TEXT("Teardown continuation captures its task"),
		Host.RetainTaskForContinuation(TeardownTask, TeardownContinuation));
	TestTrue(TEXT("Teardown caller transfers its reference"),
		Host.ReleaseTaskResult(TeardownTask));
	Owner->Teardown();
	TestEqual(TEXT("Teardown retires captured running task"), Tasks.GetCount(), 0);
	TestEqual(TEXT("Teardown retires continuation"), Owner->GetActiveCount(), 0);
	return true;
}

#endif
