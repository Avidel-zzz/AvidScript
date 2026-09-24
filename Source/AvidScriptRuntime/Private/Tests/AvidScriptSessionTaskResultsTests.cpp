#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptSessionContinuations.h"

#include "Misc/AutomationTest.h"

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

#endif
