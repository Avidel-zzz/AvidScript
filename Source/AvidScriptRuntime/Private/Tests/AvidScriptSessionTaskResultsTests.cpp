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
	TestTrue(TEXT("Active waiter queues"),
		Active.RegisterTaskWaiter(ActiveTask, 101)
			== EAvidScriptTaskWaitRegistration::Queued);

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
	TestEqual(TEXT("Waiter identity is preserved"), Woken[0], 101LL);
	TestTrue(TEXT("Active result is repeatable"), Active.ReadTaskResult(ActiveTask, Snapshot));
	TestTrue(TEXT("Active result is successful"),
		Snapshot.State == EAvidScriptTaskResultState::Succeeded);
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

#endif
