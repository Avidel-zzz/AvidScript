#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptSessionContinuations.h"

#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

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

#endif
