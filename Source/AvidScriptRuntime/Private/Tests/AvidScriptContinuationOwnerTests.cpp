#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptAsyncObjectLoader.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "AvidScriptWasmRuntime.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"

#include "Containers/StringConv.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

namespace
{
class FAvidScriptTestPendingLatentAction final : public FPendingLatentAction
{
public:
	explicit FAvidScriptTestPendingLatentAction(
		const FAvidScriptBindingLatentReservation& Reservation)
		: ExecutionFunction(Reservation.ExecutionFunction)
		, Linkage(Reservation.Linkage)
		, CallbackTarget(Reservation.CallbackTarget)
	{
	}

	virtual void UpdateOperation(FLatentResponse& Response) override
	{
		Response.FinishAndTriggerIf(
			true,
			ExecutionFunction,
			Linkage,
			CallbackTarget);
	}

private:
	FName ExecutionFunction;
	int32 Linkage = INDEX_NONE;
	FWeakObjectPtr CallbackTarget;
};

class FAvidScriptContinuationHostSpy final : public IAvidScriptContinuationHost
{
public:
	int64 ScheduleDelay(const float DelaySeconds, const int32 CallbackId) override
	{
		LastDelaySeconds = DelaySeconds;
		LastCallbackId = CallbackId;
		++ScheduleCount;
		return ExpectedToken;
	}

	int64 ScheduleObjectLoad(
		FString ObjectPath,
		const int32 CallbackId) override
	{
		LastObjectPath = MoveTemp(ObjectPath);
		LastCallbackId = CallbackId;
		++ObjectLoadScheduleCount;
		return ExpectedToken;
	}

	bool Cancel(const int64 Token) override
	{
		LastCancelledToken = Token;
		++CancelCount;
		return Token == ExpectedToken;
	}

	int64 CreateCancellationSource() override
	{
		++CreateCancellationSourceCount;
		return ExpectedCancellationSourceToken;
	}

	bool CancelCancellationSource(const int64 SourceToken) override
	{
		LastCancellationSourceToken = SourceToken;
		++CancelCancellationSourceCount;
		return SourceToken == ExpectedCancellationSourceToken;
	}

	bool ReleaseCancellationSource(const int64 SourceToken) override
	{
		LastCancellationSourceToken = SourceToken;
		++ReleaseCancellationSourceCount;
		return SourceToken == ExpectedCancellationSourceToken;
	}

	bool BindCancellationSource(
		const int64 SourceToken,
		const int64 ContinuationToken) override
	{
		LastCancellationSourceToken = SourceToken;
		LastBoundContinuationToken = ContinuationToken;
		++BindCancellationSourceCount;
		return SourceToken == ExpectedCancellationSourceToken
			&& ContinuationToken == ExpectedToken;
	}

	static constexpr int64 ExpectedToken = 0x000000010000002aLL;
	static constexpr int64 ExpectedCancellationSourceToken =
		static_cast<int64>(0x800000010000002bULL);
	float LastDelaySeconds = 0.0f;
	FString LastObjectPath;
	int32 LastCallbackId = 0;
	int64 LastCancelledToken = 0;
	int64 LastCancellationSourceToken = 0;
	int64 LastBoundContinuationToken = 0;
	int32 ScheduleCount = 0;
	int32 ObjectLoadScheduleCount = 0;
	int32 CancelCount = 0;
	int32 CreateCancellationSourceCount = 0;
	int32 CancelCancellationSourceCount = 0;
	int32 ReleaseCancellationSourceCount = 0;
	int32 BindCancellationSourceCount = 0;
};

struct FAvidScriptFakeAsyncLoadCancelState
{
	int32 CancelCount = 0;
	bool bCancelled = false;
};

class FAvidScriptFakeAsyncLoadHandle final
	: public IAvidScriptAsyncObjectLoadHandle
{
public:
	explicit FAvidScriptFakeAsyncLoadHandle(
		TSharedRef<FAvidScriptFakeAsyncLoadCancelState> InState)
		: State(MoveTemp(InState))
	{
	}

	virtual void Cancel() override
	{
		if (!State->bCancelled)
		{
			State->bCancelled = true;
			++State->CancelCount;
		}
	}

private:
	TSharedRef<FAvidScriptFakeAsyncLoadCancelState> State;
};

class FAvidScriptFakeAsyncObjectLoader final
	: public IAvidScriptAsyncObjectLoader
{
public:
	struct FRequest
	{
		FSoftObjectPath ObjectPath;
		FCompletion Completion;
		TSharedRef<FAvidScriptFakeAsyncLoadCancelState> CancelState =
			MakeShared<FAvidScriptFakeAsyncLoadCancelState>();
	};

	virtual TSharedPtr<IAvidScriptAsyncObjectLoadHandle> RequestAsyncLoad(
		const FSoftObjectPath& ObjectPath,
		FCompletion&& Completion) override
	{
		TSharedRef<FRequest> Request = MakeShared<FRequest>();
		Request->ObjectPath = ObjectPath;
		Request->Completion = MoveTemp(Completion);
		Requests.Add(Request);
		TSharedPtr<IAvidScriptAsyncObjectLoadHandle> Handle =
			MakeShared<FAvidScriptFakeAsyncLoadHandle>(Request->CancelState);
		if (bCompleteSynchronously)
		{
			FCompletion LocalCompletion = MoveTemp(Request->Completion);
			LocalCompletion(SynchronousObject);
			if (AfterSynchronousCompletion)
			{
				AfterSynchronousCompletion();
			}
		}
		return bReturnHandle ? MoveTemp(Handle) : nullptr;
	}

	void Complete(const int32 RequestIndex, UObject* LoadedObject)
	{
		if (!Requests.IsValidIndex(RequestIndex)
			|| !Requests[RequestIndex]->Completion)
		{
			return;
		}
		FCompletion LocalCompletion =
			MoveTemp(Requests[RequestIndex]->Completion);
		LocalCompletion(LoadedObject);
	}

	TArray<TSharedRef<FRequest>> Requests;
	TFunction<void()> AfterSynchronousCompletion;
	UObject* SynchronousObject = nullptr;
	bool bCompleteSynchronously = false;
	bool bReturnHandle = true;
};

class FAvidScriptTestLatentCompletionProvider final
	: public IAvidScriptLatentCompletionProvider
{
public:
	FString GetProviderId() const override
	{
		return TEXT("avidscript.runtime.test.i32.v1");
	}

	FString GetFunctionPath() const override
	{
		return TEXT("/Script/AvidScriptRuntime.TestLatentPayload");
	}

	FString GetPayloadTypeId() const override
	{
		return FString::ChrN(64, TEXT('1'));
	}

	void Publish(UObject* CallbackTarget, const int32 UUID, const int32 Value)
	{
		PendingTarget = CallbackTarget;
		PendingUUID = UUID;
		PendingValue = Value;
	}

	bool ConsumePayload(
		UObject* CallbackTarget,
		const int32 UUID,
		FAvidScriptBindingLatentCompletionPayload& OutPayload) override
	{
		OutPayload = {};
		if (PendingTarget.Get() != CallbackTarget
			|| PendingUUID != UUID
			|| !PendingValue.IsSet())
		{
			return false;
		}
		OutPayload.TypeId = GetPayloadTypeId();
		OutPayload.AbiCells.Add(
			static_cast<uint64>(static_cast<int64>(PendingValue.GetValue())));
		PendingTarget.Reset();
		PendingUUID = INDEX_NONE;
		PendingValue.Reset();
		++ConsumeCount;
		return true;
	}

	void AbandonPayload(UObject* CallbackTarget, const int32 UUID) override
	{
		if (PendingTarget.Get() == CallbackTarget && PendingUUID == UUID)
		{
			PendingTarget.Reset();
			PendingUUID = INDEX_NONE;
			PendingValue.Reset();
			++AbandonCount;
		}
	}

	int32 ConsumeCount = 0;
	int32 AbandonCount = 0;

private:
	TWeakObjectPtr<UObject> PendingTarget;
	TOptional<int32> PendingValue;
	int32 PendingUUID = INDEX_NONE;
};

uint32 InternContinuationUtf8(
	FAvidScriptWasmRuntimeInstance& Runtime,
	const TConstArrayView<uint8> Bytes)
{
	FAvidScriptUtf8ValueReservation Reservation;
	FString Error;
	uint32 Token = 0;
	bool bCreated = false;
	FAvidScriptUtf8ValueHeap& Heap = Runtime.GetUtf8ValueHeapForTesting();
	if (!Heap.Reserve(Reservation, Error)
		|| !Heap.InternReserved(
			Reservation,
			Bytes,
			Token,
			bCreated,
			Error))
	{
		Heap.ReleaseReservation(Reservation);
		return 0;
	}
	return Token;
}

void RegisterTestLatentAction(
	UWorld* World,
	const FAvidScriptBindingLatentReservation& Reservation)
{
	World->GetLatentActionManager().AddNewAction(
		Reservation.CallbackTarget,
		Reservation.UUID,
		new FAvidScriptTestPendingLatentAction(Reservation));
}

bool TriggerLatentReservationSynchronously(
	const FAvidScriptBindingLatentReservation& Reservation)
{
	if (!Reservation.IsValid())
	{
		return false;
	}
	UFunction* const Callback =
		Reservation.CallbackTarget->FindFunction(Reservation.ExecutionFunction);
	if (Callback == nullptr)
	{
		return false;
	}
	struct FCallbackParameters
	{
		int32 Linkage = 0;
	};
	FCallbackParameters Parameters;
	Parameters.Linkage = Reservation.Linkage;
	Reservation.CallbackTarget->ProcessEvent(Callback, &Parameters);
	return true;
}

bool CreateContinuationWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptContinuationWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	OutWorld->InitializeActorsForPlay(FURL());
	return true;
}

void DestroyContinuationWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

void AdvanceContinuationWorld(UWorld* World, const float ElapsedSeconds)
{
	World->Tick(LEVELTICK_All, 0.0f);
	++GFrameCounter;
	World->Tick(LEVELTICK_All, ElapsedSeconds);
	++GFrameCounter;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationHostBoundaryTest,
	"AvidScript.Runtime.Continuation.HostBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationHostBoundaryTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptContinuationHostSpy Host;
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext Context;
	Context.Continuations = &Host;
	Runtime.SetHostContext(Context);

	const int64 Token = Runtime.HandleContinuationDelayImport(0.25f, 17);
	TestEqual(TEXT("Runtime preserves the full i64 continuation token"), Token, Host.ExpectedToken);
	TestEqual(TEXT("Runtime forwards the delay"), Host.LastDelaySeconds, 0.25f);
	TestEqual(TEXT("Runtime forwards the callback id"), Host.LastCallbackId, 17);
	TestEqual(TEXT("Schedule crosses the host boundary once"), Host.ScheduleCount, 1);
	TestEqual(
		TEXT("Runtime forwards the full token to cancel"),
		Runtime.HandleContinuationCancelImport(Token),
		1);
	TestEqual(TEXT("Cancel crosses the host boundary once"), Host.CancelCount, 1);
	TestEqual(TEXT("Cancel receives the exact token"), Host.LastCancelledToken, Token);
	TestEqual(
		TEXT("A rejected forged token remains a normal false result"),
		Runtime.HandleContinuationCancelImport(Token + 1),
		0);

	const int64 CancellationSourceToken =
		Runtime.HandleContinuationCancelSourceCreateImport();
	TestEqual(
		TEXT("Runtime preserves the full cancellation source token"),
		CancellationSourceToken,
		Host.ExpectedCancellationSourceToken);
	TestEqual(
		TEXT("Cancellation source creation crosses the host boundary once"),
		Host.CreateCancellationSourceCount,
		1);
	TestEqual(
		TEXT("Runtime forwards source and continuation tokens to bind"),
		Runtime.HandleContinuationBindCancelImport(
			CancellationSourceToken,
			Token),
		1);
	TestEqual(TEXT("Cancellation bind crosses the host boundary once"), Host.BindCancellationSourceCount, 1);
	TestEqual(TEXT("Cancellation bind preserves the source token"), Host.LastCancellationSourceToken, CancellationSourceToken);
	TestEqual(TEXT("Cancellation bind preserves the continuation token"), Host.LastBoundContinuationToken, Token);
	TestEqual(
		TEXT("Runtime forwards source cancellation"),
		Runtime.HandleContinuationCancelSourceCancelImport(CancellationSourceToken),
		1);
	TestEqual(TEXT("Source cancellation crosses the host boundary once"), Host.CancelCancellationSourceCount, 1);
	TestEqual(
		TEXT("Runtime forwards source release"),
		Runtime.HandleContinuationCancelSourceReleaseImport(CancellationSourceToken),
		1);
	TestEqual(TEXT("Source release crosses the host boundary once"), Host.ReleaseCancellationSourceCount, 1);

	const FString ObjectPath(TEXT("/Engine/EngineMeshes/Cube.Cube"));
	const FTCHARToUTF8 ObjectPathUtf8(*ObjectPath);
	const uint32 ObjectPathToken = InternContinuationUtf8(
		Runtime,
		MakeArrayView(
			reinterpret_cast<const uint8*>(ObjectPathUtf8.Get()),
			ObjectPathUtf8.Length()));
	TestNotEqual(TEXT("Object path fixture receives a UTF-8 value token"), ObjectPathToken, 0u);
	TestEqual(
		TEXT("Runtime preserves the async object continuation token"),
		Runtime.HandleContinuationLoadObjectImport(
			static_cast<int32>(ObjectPathToken),
			23),
		Host.ExpectedToken);
	TestEqual(TEXT("Runtime forwards the decoded object path"), Host.LastObjectPath, ObjectPath);
	TestEqual(TEXT("Runtime forwards the object callback id"), Host.LastCallbackId, 23);
	TestEqual(TEXT("Object loading crosses the host boundary once"), Host.ObjectLoadScheduleCount, 1);

	const uint8 EmbeddedNullPath[] = { '/', 'E', 'n', 'g', 'i', 'n', 'e', 0, 'X' };
	const uint32 EmbeddedNullToken = InternContinuationUtf8(
		Runtime,
		MakeArrayView(EmbeddedNullPath));
	TestEqual(
		TEXT("Runtime rejects an embedded NUL object path"),
		Runtime.HandleContinuationLoadObjectImport(
			static_cast<int32>(EmbeddedNullToken),
			24),
		0LL);
	TestEqual(TEXT("Rejected object paths do not cross the host boundary"), Host.ObjectLoadScheduleCount, 1);
	FString FailureModule;
	FString FailureImport;
	FString FailureDetails;
	TestTrue(
		TEXT("Rejected object paths publish a host import failure"),
		Runtime.ConsumePendingHostImportFailure(
			FailureModule,
			FailureImport,
			FailureDetails));
	TestEqual(TEXT("Object path failure uses the env module"), FailureModule, FString(TEXT("env")));
	TestEqual(
		TEXT("Object path failure names the load import"),
		FailureImport,
		FString(TEXT("continuation_load_object")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationCancellationSourceTest,
	"AvidScript.Runtime.Continuation.CancellationSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationCancellationSourceTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Cancellation source world is created"),
			CreateContinuationWorld(World)))
	{
		return false;
	}

	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	IAvidScriptContinuationHost& ActiveHost = Owner->ResetActive(World);
	const int64 SourceToken = ActiveHost.CreateCancellationSource();
	TestNotEqual(TEXT("Cancellation source returns an opaque token"), SourceToken, 0LL);
	TestTrue(TEXT("Cancellation source uses a distinct high-bit token kind"), SourceToken < 0);
	TestEqual(TEXT("One cancellation source is tracked"), Owner->GetCancellationSourceCountForTesting(), 1);

	const int64 PendingToken = ActiveHost.ScheduleDelay(10.0f, 41);
	const int64 ReadyToken = ActiveHost.ScheduleDelay(0.01f, 42);
	TestTrue(TEXT("Pending continuation binds to the source"), ActiveHost.BindCancellationSource(SourceToken, PendingToken));
	TestTrue(TEXT("Ready fixture binds to the same source"), ActiveHost.BindCancellationSource(SourceToken, ReadyToken));
	TestEqual(TEXT("Two cancellation bindings are tracked"), Owner->GetCancellationBindingCountForTesting(), 2);
	TestFalse(TEXT("Source tokens cannot cancel continuation slots"), ActiveHost.Cancel(SourceToken));
	TestFalse(TEXT("Continuation tokens cannot cancel source slots"), ActiveHost.CancelCancellationSource(PendingToken));
	AdvanceContinuationWorld(World, 0.02f);
	TestTrue(TEXT("Source cancellation wins over pending and ready work"), ActiveHost.CancelCancellationSource(SourceToken));
	TestFalse(TEXT("Source cancellation is idempotent"), ActiveHost.CancelCancellationSource(SourceToken));
	TestEqual(TEXT("Source cancellation removes all cancellable work"), Owner->GetActiveCount(), 0);
	TestEqual(TEXT("Source cancellation removes reverse bindings"), Owner->GetCancellationBindingCountForTesting(), 0);
	TArray<FAvidScriptContinuationCompletion> Completions;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("A source-cancelled ready callback is suppressed"), Completions.Num(), 0);

	const int64 CancelledBeforeBind = ActiveHost.ScheduleDelay(10.0f, 43);
	TestTrue(
		TEXT("Binding to an already cancelled source synchronously handles the producer"),
		ActiveHost.BindCancellationSource(SourceToken, CancelledBeforeBind));
	TestEqual(TEXT("Cancel-before-bind leaves no producer"), Owner->GetActiveCount(), 0);
	const int64 InvalidBindToken = ActiveHost.ScheduleDelay(10.0f, 44);
	TestFalse(
		TEXT("Binding to an invalid source fails closed"),
		ActiveHost.BindCancellationSource(0, InvalidBindToken));
	TestEqual(TEXT("A failed bind leaves no orphan producer"), Owner->GetActiveCount(), 0);
	TestTrue(TEXT("The cancelled source can be released"), ActiveHost.ReleaseCancellationSource(SourceToken));
	TestFalse(TEXT("Released source tokens are stale"), ActiveHost.ReleaseCancellationSource(SourceToken));
	TestEqual(TEXT("Released source slots are reclaimed"), Owner->GetCancellationSourceCountForTesting(), 0);

	const int64 DispatchSource = ActiveHost.CreateCancellationSource();
	const int64 DispatchToken = ActiveHost.ScheduleDelay(0.01f, 45);
	TestTrue(TEXT("Dispatch fixture binds"), ActiveHost.BindCancellationSource(DispatchSource, DispatchToken));
	AdvanceContinuationWorld(World, 0.02f);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Dispatch fixture reaches the safe pump"), Completions.Num(), 1);
	TestTrue(TEXT("Source can enter cancelled state during dispatch"), ActiveHost.CancelCancellationSource(DispatchSource));
	TestEqual(TEXT("Dispatching completion wins the cancellation race"), Owner->GetActiveCount(), 1);
	TestEqual(TEXT("Dispatching binding remains until finalization"), Owner->GetCancellationBindingCountForTesting(), 1);
	TestTrue(TEXT("Dispatching completion finalizes"), Owner->FinalizeDispatched(DispatchToken, true));
	TestEqual(TEXT("Finalization detaches the source binding"), Owner->GetCancellationBindingCountForTesting(), 0);
	TestTrue(TEXT("Dispatch source releases after finalization"), ActiveHost.ReleaseCancellationSource(DispatchSource));

	const int64 ActiveSource = ActiveHost.CreateCancellationSource();
	const int64 ActiveToken = ActiveHost.ScheduleDelay(10.0f, 46);
	IAvidScriptContinuationHost& PreparedHost = Owner->BeginPrepared(World);
	const int64 PreparedSource = PreparedHost.CreateCancellationSource();
	TestFalse(
		TEXT("Prepared source cannot bind an active continuation"),
		ActiveHost.BindCancellationSource(PreparedSource, ActiveToken));
	TestEqual(TEXT("Cross-lane bind failure cancels only its active producer"), Owner->GetActiveCount(), 0);
	TestEqual(TEXT("Prepared source remains isolated"), Owner->GetCancellationSourceCountForTesting(), 2);
	Owner->DiscardPrepared();
	TestEqual(TEXT("Discarding prepared state invalidates its source"), Owner->GetCancellationSourceCountForTesting(), 1);
	TestFalse(TEXT("Discarded source token is stale"), ActiveHost.CancelCancellationSource(PreparedSource));
	TestTrue(TEXT("Active source survives candidate discard"), ActiveHost.ReleaseCancellationSource(ActiveSource));

	Owner->Teardown();
	TestEqual(TEXT("Teardown leaves no cancellation sources"), Owner->GetCancellationSourceCountForTesting(), 0);
	TestEqual(TEXT("Teardown leaves no cancellation bindings"), Owner->GetCancellationBindingCountForTesting(), 0);
	DestroyContinuationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationActiveLifecycleTest,
	"AvidScript.Runtime.Continuation.ActiveLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationActiveLifecycleTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Continuation test world is created"), CreateContinuationWorld(World)))
	{
		return false;
	}

	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	IAvidScriptContinuationHost& Host = Owner->ResetActive(World);
	const int64 CancelledToken = Host.ScheduleDelay(10.0f, 1);
	TestNotEqual(TEXT("A delay returns an opaque token"), CancelledToken, 0LL);
	TestEqual(TEXT("One active continuation is tracked"), Owner->GetActiveCount(), 1);
	TestTrue(TEXT("The active token can be cancelled"), Host.Cancel(CancelledToken));
	TestFalse(TEXT("A cancelled token is stale"), Host.Cancel(CancelledToken));

	const int64 FirstToken = Host.ScheduleDelay(0.01f, 11);
	const int64 SecondToken = Host.ScheduleDelay(0.01f, 12);
	TestNotEqual(TEXT("First ordered token is valid"), FirstToken, 0LL);
	TestNotEqual(TEXT("Second ordered token is valid"), SecondToken, 0LL);
	AdvanceContinuationWorld(World, 0.02f);

	TArray<FAvidScriptContinuationCompletion> Completions;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("One continuation is delivered per safe pump"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Equal deadlines preserve registration order"), Completions[0].CallbackId, 11);
		TestFalse(TEXT("A dispatching continuation cannot cancel itself"), Host.Cancel(FirstToken));
		TestTrue(TEXT("The first dispatch finalizes after Guest return"), Owner->FinalizeDispatched(FirstToken, true));
	}
	Owner->DrainReady(Completions);
	TestEqual(TEXT("The second safe pump delivers the next continuation"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Second completion follows the first"), Completions[0].CallbackId, 12);
		TestTrue(TEXT("The second dispatch finalizes after Guest return"), Owner->FinalizeDispatched(SecondToken, true));
	}
	TestFalse(TEXT("Completed tokens cannot be cancelled"), Host.Cancel(FirstToken));
	TestEqual(TEXT("Completed continuations leave no active entries"), Owner->GetActiveCount(), 0);
	const int64 ReadyToken = Host.ScheduleDelay(0.01f, 13);
	AdvanceContinuationWorld(World, 0.02f);
	TestTrue(TEXT("Cancellation can suppress a ready but not yet delivered callback"), Host.Cancel(ReadyToken));
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Cancelled ready callback is not delivered"), Completions.Num(), 0);

	Owner->Teardown();
	DestroyContinuationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationPreparedTransactionTest,
	"AvidScript.Runtime.Continuation.PreparedTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationPreparedTransactionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Continuation transaction world is created"), CreateContinuationWorld(World)))
	{
		return false;
	}

	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	IAvidScriptContinuationHost& ActiveHost = Owner->ResetActive(World);
	const int64 ActiveToken = ActiveHost.ScheduleDelay(10.0f, 21);
	IAvidScriptContinuationHost& RejectedCandidate = Owner->BeginPrepared(World);
	const int64 RejectedToken = RejectedCandidate.ScheduleDelay(10.0f, 22);
	TestNotEqual(TEXT("Rejected candidate initially receives a token"), RejectedToken, 0LL);
	TestEqual(TEXT("Candidate uses an isolated prepared lane"), Owner->GetPreparedCount(), 1);
	Owner->DiscardPrepared();
	TestEqual(TEXT("Rejected candidate entries are discarded"), Owner->GetPreparedCount(), 0);
	TestEqual(TEXT("Rejected candidate preserves the active lane"), Owner->GetActiveCount(), 1);
	TestTrue(TEXT("Old active token remains cancellable"), ActiveHost.Cancel(ActiveToken));

	IAvidScriptContinuationHost& CandidateHost = Owner->BeginPrepared(World);
	const int64 CandidateToken = CandidateHost.ScheduleDelay(0.01f, 31);
	AdvanceContinuationWorld(World, 0.02f);
	FString ValidationError;
	TestTrue(
		TEXT("Completed prepared work remains commit-valid"),
		Owner->ValidatePreparedCommit(ValidationError));
	Owner->CommitPrepared();
	TArray<FAvidScriptContinuationCompletion> Completions;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Committed prepared completion is promoted once"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Promoted completion keeps its callback id"), Completions[0].CallbackId, 31);
		TestEqual(TEXT("Promoted completion keeps its token"), Completions[0].Token, CandidateToken);
		TestFalse(TEXT("A promoted dispatch cannot cancel itself"), CandidateHost.Cancel(CandidateToken));
		TestTrue(TEXT("Promoted work finalizes after Guest return"), Owner->FinalizeDispatched(CandidateToken, true));
	}
	TestFalse(TEXT("Completed promoted token is stale"), CandidateHost.Cancel(CandidateToken));

	Owner->Teardown();
	DestroyContinuationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptAsyncObjectContinuationTest,
	"AvidScript.Runtime.Continuation.AsyncObjectProducer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptAsyncObjectContinuationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Async object continuation world is created"),
			CreateContinuationWorld(World)))
	{
		return false;
	}
	const TSharedPtr<FAvidScriptFakeAsyncObjectLoader> Loader =
		MakeShared<FAvidScriptFakeAsyncObjectLoader>();
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>(Loader);
	FAvidScriptObjectRegistry Registry;
	FAvidScriptSessionObjectOwnership Ownership;
	IAvidScriptContinuationHost& InitialHost = Owner->ResetActive(
		World,
		&Registry,
		&Ownership);
	const FString ObjectPath(TEXT("/Engine/EngineMeshes/Cube.Cube"));
	UObject* const LoadedObject =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());

	const int64 InitialToken = InitialHost.ScheduleObjectLoad(ObjectPath, 101);
	TestNotEqual(TEXT("Async object scheduling returns an opaque token"), InitialToken, 0LL);
	TestEqual(TEXT("The fake loader receives one request"), Loader->Requests.Num(), 1);
	Loader->Complete(0, LoadedObject);

	TArray<FAvidScriptContinuationCompletion> Completions;
	Owner->DrainReady(Completions);
	if (TestEqual(TEXT("A completed object load drains once"), Completions.Num(), 1))
	{
		const FAvidScriptContinuationCompletion& Completion = Completions[0];
		TestEqual(TEXT("The completed object load preserves its callback"), Completion.CallbackId, 101);
		TestEqual(TEXT("The object load reports Completed"), Completion.Status, EAvidScriptContinuationStatus::Completed);
		TestTrue(TEXT("The completion publishes an object slot"), Completion.ObjectSlot > 0);
		TestTrue(TEXT("The completion publishes an object generation"), Completion.ObjectGeneration > 0);
		FAvidScriptObjectHandleResult ResolveResult;
		TestEqual(
			TEXT("The dispatch-only borrowed handle resolves to the loaded object"),
			Registry.ResolveObject(
				{
					static_cast<uint32>(Completion.ObjectSlot),
					static_cast<uint32>(Completion.ObjectGeneration)
				},
				ResolveResult,
				false),
			LoadedObject);
		TestFalse(TEXT("The dispatching object continuation cannot cancel itself"), InitialHost.Cancel(InitialToken));
		TestTrue(TEXT("A successful object callback finalizes"), Owner->FinalizeDispatched(InitialToken, true));
	}
	TestEqual(TEXT("Successful dispatch retains one unique loaded object"), Owner->GetRetainedLoadedObjectCountForTesting(), 1);
	TestEqual(TEXT("Successful dispatch leaves one borrowed capability"), Ownership.GetBorrowedHandleCount(), 1);

	IAvidScriptContinuationHost& ReloadedHost = Owner->BeginPrepared(
		World,
		&Registry,
		&Ownership);
	const int64 ReloadedToken = ReloadedHost.ScheduleObjectLoad(ObjectPath, 102);
	const int32 ReloadedRequestIndex = Loader->Requests.Num() - 1;
	Loader->Complete(ReloadedRequestIndex, LoadedObject);
	FString CommitError;
	TestTrue(TEXT("A prepared object completion is commit-valid"), Owner->ValidatePreparedCommit(CommitError));
	Owner->CommitPrepared();
	Owner->DrainReady(Completions);
	if (TestEqual(TEXT("A committed prepared object completion drains"), Completions.Num(), 1))
	{
		TestTrue(TEXT("The committed object completion finalizes"), Owner->FinalizeDispatched(ReloadedToken, true));
	}
	TestEqual(TEXT("Successful hot reload preserves and deduplicates retained objects"), Owner->GetRetainedLoadedObjectCountForTesting(), 1);
	TestEqual(TEXT("Duplicate loaded objects reuse the borrowed capability"), Ownership.GetBorrowedHandleCount(), 1);

	const int64 FailedToken = ReloadedHost.ScheduleObjectLoad(ObjectPath, 103);
	Loader->Complete(Loader->Requests.Num() - 1, nullptr);
	Owner->DrainReady(Completions);
	if (TestEqual(TEXT("A failed producer still drains one callback"), Completions.Num(), 1))
	{
		TestEqual(TEXT("Null producer output reports Failed"), Completions[0].Status, EAvidScriptContinuationStatus::Failed);
		TestEqual(TEXT("Failed output has no object slot"), Completions[0].ObjectSlot, 0);
		TestEqual(TEXT("Failed output has no object generation"), Completions[0].ObjectGeneration, 0);
		TestTrue(TEXT("The failed producer callback finalizes normally"), Owner->FinalizeDispatched(FailedToken, true));
	}

	UObject* const RolledBackObject =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	const int32 BorrowCheckpoint = Ownership.GetBorrowedHandleCount();
	const int64 RolledBackToken = ReloadedHost.ScheduleObjectLoad(ObjectPath, 104);
	Loader->Complete(Loader->Requests.Num() - 1, RolledBackObject);
	Owner->DrainReady(Completions);
	FAvidScriptObjectHandle RolledBackHandle;
	if (TestEqual(TEXT("The rollback fixture drains one completion"), Completions.Num(), 1))
	{
		RolledBackHandle = {
			static_cast<uint32>(Completions[0].ObjectSlot),
			static_cast<uint32>(Completions[0].ObjectGeneration)
		};
		TestTrue(TEXT("A trapped object callback finalizes with rollback"), Owner->FinalizeDispatched(RolledBackToken, false));
	}
	TestEqual(TEXT("Trap rollback restores the borrowed-handle checkpoint"), Ownership.GetBorrowedHandleCount(), BorrowCheckpoint);
	FAvidScriptObjectHandleResult RolledBackResolve;
	TestNull(
		TEXT("Trap rollback revokes the newly borrowed loaded-object handle"),
		Registry.ResolveObject(RolledBackHandle, RolledBackResolve, false));
	TestEqual(TEXT("Trapped callbacks do not retain another loaded object"), Owner->GetRetainedLoadedObjectCountForTesting(), 1);

	const int64 CancelledToken = ReloadedHost.ScheduleObjectLoad(ObjectPath, 105);
	const int32 CancelledRequestIndex = Loader->Requests.Num() - 1;
	TestTrue(TEXT("A pending object load can be cancelled"), ReloadedHost.Cancel(CancelledToken));
	TestEqual(TEXT("Cancellation reaches the producer handle exactly once"), Loader->Requests[CancelledRequestIndex]->CancelState->CancelCount, 1);
	Loader->Complete(CancelledRequestIndex, LoadedObject);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("A late completion after cancellation is suppressed"), Completions.Num(), 0);

	IAvidScriptContinuationHost& DiscardedHost = Owner->BeginPrepared(
		World,
		&Registry,
		&Ownership);
	const int64 DiscardedToken = DiscardedHost.ScheduleObjectLoad(ObjectPath, 106);
	Loader->Complete(Loader->Requests.Num() - 1, LoadedObject);
	TestNotEqual(TEXT("Prepared object work initially has a token"), DiscardedToken, 0LL);
	Owner->DiscardPrepared();
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Discarded prepared object work never reaches Guest"), Completions.Num(), 0);
	TestEqual(TEXT("Discarding prepared work preserves delivered session assets"), Owner->GetRetainedLoadedObjectCountForTesting(), 1);

	Loader->bCompleteSynchronously = true;
	Loader->SynchronousObject = LoadedObject;
	Loader->bReturnHandle = false;
	const int64 NullHandleToken = ReloadedHost.ScheduleObjectLoad(ObjectPath, 107);
	TestEqual(TEXT("A synchronous completion without a producer handle rejects its token"), NullHandleToken, 0LL);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("A null producer handle removes the synchronously queued ready item"), Completions.Num(), 0);
	TestEqual(TEXT("A null producer handle releases the continuation entry"), Owner->GetActiveCount(), 0);

	Loader->bReturnHandle = true;
	Loader->AfterSynchronousCompletion = [Owner]()
	{
		Owner->Teardown();
	};
	const int64 SynchronousToken = ReloadedHost.ScheduleObjectLoad(ObjectPath, 108);
	const int32 SynchronousRequestIndex = Loader->Requests.Num() - 1;
	TestEqual(TEXT("Synchronous completion followed by teardown rejects the stale token"), SynchronousToken, 0LL);
	TestEqual(TEXT("The returned synchronous producer handle is cancelled once"), Loader->Requests[SynchronousRequestIndex]->CancelState->CancelCount, 1);

	Ownership.Cleanup(Registry);
	DestroyContinuationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationActivationLivenessTest,
	"AvidScript.Runtime.Continuation.ActivationLiveness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationActivationLivenessTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Activation liveness world is created"),
			CreateContinuationWorld(World)))
	{
		return false;
	}
	{
		const TSharedPtr<FAvidScriptSessionContinuations> HeadlessOwner =
			MakeShared<FAvidScriptSessionContinuations>();
		HeadlessOwner->BeginPrepared(nullptr);
		FString HeadlessCommitError;
		TestTrue(
			TEXT("A headless activation without continuation entries can commit"),
			HeadlessOwner->ValidatePreparedCommit(HeadlessCommitError));
		HeadlessOwner->CommitPrepared();
		HeadlessOwner->Teardown();
	}

	const FString ObjectPath(TEXT("/Engine/EngineMeshes/Cube.Cube"));
	const TSharedPtr<FAvidScriptFakeAsyncObjectLoader> Loader =
		MakeShared<FAvidScriptFakeAsyncObjectLoader>();
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>(Loader);
	FAvidScriptObjectRegistry Registry;
	FAvidScriptSessionObjectOwnership Ownership;
	UObject* const FirstOwner =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle FirstOwnerHandle = Registry.RegisterObject(
		FirstOwner,
		RegisterResult,
		false);
	TestTrue(TEXT("First activation owner registers"), FirstOwnerHandle.IsValid());

	IAvidScriptContinuationHost& FirstHost = Owner->ResetActive(
		World,
		&Registry,
		&Ownership,
		FirstOwnerHandle);
	const int64 ReadyTimerToken = FirstHost.ScheduleDelay(0.01f, 201);
	const int64 PendingLoadToken = FirstHost.ScheduleObjectLoad(ObjectPath, 202);
	const int32 PendingLoadIndex = Loader->Requests.Num() - 1;
	TestNotEqual(TEXT("Ready-timer fixture receives a token"), ReadyTimerToken, 0LL);
	TestNotEqual(TEXT("Pending-load fixture receives a token"), PendingLoadToken, 0LL);
	AdvanceContinuationWorld(World, 0.02f);

	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(
		TEXT("Owner generation can be invalidated before dispatch"),
		Registry.ReleaseHandle(FirstOwnerHandle, ReleaseResult, false));
	TArray<FAvidScriptContinuationCompletion> Completions;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Invalid owner suppresses a ready timer"), Completions.Num(), 0);
	TestEqual(TEXT("Invalid owner cancels all active entries"), Owner->GetActiveCount(), 0);
	TestEqual(
		TEXT("Invalid owner cancels a pending load exactly once"),
		Loader->Requests[PendingLoadIndex]->CancelState->CancelCount,
		1);
	TestEqual(
		TEXT("A stale endpoint cannot schedule after owner invalidation"),
		FirstHost.ScheduleDelay(0.0f, 203),
		0LL);
	Loader->Complete(PendingLoadIndex, FirstOwner);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Late completion after owner invalidation is suppressed"), Completions.Num(), 0);

	UObject* const SecondOwner =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	const FAvidScriptObjectHandle SecondOwnerHandle = Registry.RegisterObject(
		SecondOwner,
		RegisterResult,
		false);
	IAvidScriptContinuationHost& SecondHost = Owner->ResetActive(
		World,
		&Registry,
		&Ownership,
		SecondOwnerHandle);
	const int64 ReboundLoadToken = SecondHost.ScheduleObjectLoad(ObjectPath, 204);
	const int32 ReboundLoadIndex = Loader->Requests.Num() - 1;
	TestNotEqual(TEXT("Rebind fixture receives a token"), ReboundLoadToken, 0LL);

	UObject* const ThirdOwner =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	const FAvidScriptObjectHandle ThirdOwnerHandle = Registry.RegisterObject(
		ThirdOwner,
		RegisterResult,
		false);
	IAvidScriptContinuationHost& ThirdHost = Owner->ResetActive(
		World,
		&Registry,
		&Ownership,
		ThirdOwnerHandle);
	TestEqual(
		TEXT("HostContext rebind cancels the old producer exactly once"),
		Loader->Requests[ReboundLoadIndex]->CancelState->CancelCount,
		1);
	TestEqual(
		TEXT("The retired endpoint rejects new work"),
		SecondHost.ScheduleDelay(0.0f, 205),
		0LL);

	IAvidScriptContinuationHost& PreparedHost = Owner->BeginPrepared(
		World,
		&Registry,
		&Ownership,
		ThirdOwnerHandle);
	const int64 PreparedLoadToken =
		PreparedHost.ScheduleObjectLoad(ObjectPath, 206);
	const int32 PreparedLoadIndex = Loader->Requests.Num() - 1;
	TestNotEqual(TEXT("Prepared fixture receives a token"), PreparedLoadToken, 0LL);
	TestTrue(
		TEXT("Prepared owner generation can be invalidated"),
		Registry.ReleaseHandle(ThirdOwnerHandle, ReleaseResult, false));
	FString CommitError;
	TestFalse(
		TEXT("Prepared commit rejects a dead activation"),
		Owner->ValidatePreparedCommit(CommitError));
	TestEqual(
		TEXT("Prepared rejection reports the liveness fence"),
		CommitError,
		FString(TEXT("continuation_prepared_context_unavailable")));
	Owner->DiscardPrepared();
	TestEqual(
		TEXT("Prepared rejection cancels its producer exactly once"),
		Loader->Requests[PreparedLoadIndex]->CancelState->CancelCount,
		1);
	Loader->Complete(PreparedLoadIndex, ThirdOwner);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Late prepared completion never reaches Guest"), Completions.Num(), 0);

	UObject* const WorldFenceOwner =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	const FAvidScriptObjectHandle WorldFenceHandle = Registry.RegisterObject(
		WorldFenceOwner,
		RegisterResult,
		false);
	IAvidScriptContinuationHost& WorldFenceHost = Owner->ResetActive(
		World,
		&Registry,
		&Ownership,
		WorldFenceHandle);
	const int64 WorldLoadToken =
		WorldFenceHost.ScheduleObjectLoad(ObjectPath, 207);
	const int32 WorldLoadIndex = Loader->Requests.Num() - 1;
	TestNotEqual(TEXT("World teardown fixture receives a token"), WorldLoadToken, 0LL);
	World->bIsTearingDown = true;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("World teardown suppresses dispatch"), Completions.Num(), 0);
	TestEqual(
		TEXT("World teardown cancels a pending producer exactly once"),
		Loader->Requests[WorldLoadIndex]->CancelState->CancelCount,
		1);
	TestEqual(
		TEXT("World teardown rejects new scheduling"),
		WorldFenceHost.ScheduleDelay(0.0f, 208),
		0LL);
	World->bIsTearingDown = false;
	Loader->Complete(WorldLoadIndex, WorldFenceOwner);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Late world-teardown completion is suppressed"), Completions.Num(), 0);

	Owner->Teardown();
	Ownership.Cleanup(Registry);
	DestroyContinuationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationLatentProducerTest,
	"AvidScript.Runtime.Continuation.LatentProducer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationLatentProducerTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Latent producer world is created"),
			CreateContinuationWorld(World)))
	{
		return false;
	}

	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	FAvidScriptContinuationHostEndpoint& ActiveHost =
		Owner->ResetActive(World);
	TArray<FAvidScriptContinuationCompletion> Completions;

	FAvidScriptBindingLatentReservation MissingAction;
	TestTrue(
		TEXT("A latent reservation can be allocated"),
		ActiveHost.BeginLatent(301, MissingAction));
	TestTrue(TEXT("The latent reservation is complete"), MissingAction.IsValid());
	TestFalse(
		TEXT("A reservation without a registered action cannot commit"),
		ActiveHost.CommitLatent(MissingAction.Token));
	TestTrue(
		TEXT("An uncommitted reservation can be aborted"),
		ActiveHost.AbortLatent(MissingAction.Token));

	FAvidScriptBindingLatentReservation Synchronous;
	TestTrue(
		TEXT("A synchronous latent reservation can be allocated"),
		ActiveHost.BeginLatent(302, Synchronous));
	TestTrue(
		TEXT("A synchronous callback can complete before commit"),
		TriggerLatentReservationSynchronously(Synchronous));
	TestTrue(
		TEXT("A synchronously completed reservation commits"),
		ActiveHost.CommitLatent(Synchronous.Token));
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Synchronous completion queues once"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Synchronous callback id is preserved"), Completions[0].CallbackId, 302);
		TestTrue(
			TEXT("Synchronous completion finalizes"),
			Owner->FinalizeDispatched(Completions[0].Token, true));
	}

	FAvidScriptBindingLatentReservation Cancelled;
	FAvidScriptBindingLatentReservation Completed;
	TestTrue(TEXT("First concurrent latent reserves"), ActiveHost.BeginLatent(303, Cancelled));
	TestTrue(TEXT("Second concurrent latent reserves"), ActiveHost.BeginLatent(304, Completed));
	TestNotEqual(
		TEXT("Concurrent awaits use distinct callback targets"),
		Cancelled.CallbackTarget,
		Completed.CallbackTarget);
	RegisterTestLatentAction(World, Cancelled);
	RegisterTestLatentAction(World, Completed);
	TestTrue(TEXT("First concurrent latent commits"), ActiveHost.CommitLatent(Cancelled.Token));
	TestTrue(TEXT("Second concurrent latent commits"), ActiveHost.CommitLatent(Completed.Token));
	TestTrue(TEXT("One concurrent latent can cancel independently"), ActiveHost.Cancel(Cancelled.Token));
	AdvanceContinuationWorld(World, 0.01f);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Only the uncancelled latent completes"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Uncancelled callback id is preserved"), Completions[0].CallbackId, 304);
		TestTrue(
			TEXT("Uncancelled latent finalizes"),
			Owner->FinalizeDispatched(Completions[0].Token, true));
	}
	TestEqual(
		TEXT("Cancelled proxy owns no remaining action"),
		World->GetLatentActionManager().GetNumActionsForObject(Cancelled.CallbackTarget),
		0);

	FAvidScriptContinuationHostEndpoint& PreparedHost =
		Owner->BeginPrepared(World);
	FAvidScriptBindingLatentReservation Promoted;
	TestTrue(TEXT("Prepared latent reserves"), PreparedHost.BeginLatent(305, Promoted));
	RegisterTestLatentAction(World, Promoted);
	TestTrue(TEXT("Prepared latent commits"), PreparedHost.CommitLatent(Promoted.Token));
	FString CommitError;
	TestTrue(
		TEXT("A committed prepared latent validates"),
		Owner->ValidatePreparedCommit(CommitError));
	Owner->CommitPrepared();
	AdvanceContinuationWorld(World, 0.01f);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Promoted latent completes on the active lane"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Promoted callback id is preserved"), Completions[0].CallbackId, 305);
		TestTrue(
			TEXT("Promoted latent finalizes"),
			Owner->FinalizeDispatched(Completions[0].Token, true));
	}

	FAvidScriptContinuationHostEndpoint& DiscardedHost =
		Owner->BeginPrepared(World);
	FAvidScriptBindingLatentReservation Discarded;
	TestTrue(TEXT("Discarded latent reserves"), DiscardedHost.BeginLatent(306, Discarded));
	RegisterTestLatentAction(World, Discarded);
	TestTrue(TEXT("Discarded latent commits"), DiscardedHost.CommitLatent(Discarded.Token));
	Owner->DiscardPrepared();
	AdvanceContinuationWorld(World, 0.01f);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Discarded latent never reaches Guest"), Completions.Num(), 0);

	FAvidScriptContinuationHostEndpoint& WorldFenceHost =
		Owner->ResetActive(World);
	FAvidScriptBindingLatentReservation WorldFence;
	TestTrue(TEXT("World-fenced latent reserves"), WorldFenceHost.BeginLatent(307, WorldFence));
	RegisterTestLatentAction(World, WorldFence);
	TestTrue(TEXT("World-fenced latent commits"), WorldFenceHost.CommitLatent(WorldFence.Token));
	World->bIsTearingDown = true;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("World teardown cancels latent dispatch"), Completions.Num(), 0);
	TestEqual(TEXT("World teardown clears the latent entry"), Owner->GetActiveCount(), 0);
	World->bIsTearingDown = false;
	AdvanceContinuationWorld(World, 0.01f);
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Late latent completion stays suppressed"), Completions.Num(), 0);

	Owner->Teardown();
	DestroyContinuationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationLatentResultSlotTest,
	"AvidScript.Runtime.Continuation.LatentResultSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationLatentResultSlotTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("Latent result world is created"),
		CreateContinuationWorld(World)))
	{
		return false;
	}
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	FAvidScriptContinuationHostEndpoint& ActiveHost =
		Owner->ResetActive(World);
	const TSharedRef<FAvidScriptTestLatentCompletionProvider> Provider =
		MakeShared<FAvidScriptTestLatentCompletionProvider>();
	FAvidScriptBindingLatentCompletionContract Contract;
	Contract.Mode = TEXT("provider");
	Contract.ProviderId = Provider->GetProviderId();
	Contract.PayloadTypeId = Provider->GetPayloadTypeId();
	Contract.StatusPolicy = TEXT("abandon_on_cancel");
	Contract.bCancellable = true;
	Contract.Provider = Provider;

	FAvidScriptBindingLatentReservation Completed;
	TestTrue(
		TEXT("Provider latent reservation allocates"),
		ActiveHost.BeginLatentWithCompletion(401, Contract, Completed));
	Provider->Publish(Completed.CallbackTarget, Completed.UUID, 42);
	TestTrue(
		TEXT("Provider may publish before synchronous completion"),
		TriggerLatentReservationSynchronously(Completed));
	TestTrue(
		TEXT("Provider latent reservation commits"),
		ActiveHost.CommitLatent(Completed.Token));
	TestEqual(
		TEXT("Committed provider result owns one bounded slot"),
		Owner->GetResultSlotCountForTesting(),
		1);

	TArray<FAvidScriptContinuationCompletion> Completions;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Provider completion drains once"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		const FAvidScriptContinuationCompletion& Completion = Completions[0];
		TestEqual(
			TEXT("Provider completion reports success"),
			Completion.Status,
			EAvidScriptContinuationStatus::Completed);
		TestTrue(TEXT("Provider completion carries a result slot"), Completion.ObjectSlot > 0);
		TestTrue(TEXT("Provider completion carries a result generation"), Completion.ObjectGeneration > 0);
		TArray<uint64> Cells;
		TestFalse(
			TEXT("Wrong payload type cannot consume the slot"),
			Owner->ConsumeResult(
				EAvidScriptContinuationLane::Active,
				ActiveHost.GetActivationSerial(),
				Completion.ObjectSlot,
				Completion.ObjectGeneration,
				FString::ChrN(64, TEXT('2')),
				Cells));
		TestTrue(
			TEXT("Exact payload type consumes the slot once"),
			Owner->ConsumeResult(
				EAvidScriptContinuationLane::Active,
				ActiveHost.GetActivationSerial(),
				Completion.ObjectSlot,
				Completion.ObjectGeneration,
				Provider->GetPayloadTypeId(),
				Cells));
		TestEqual(TEXT("One payload cell is returned"), Cells.Num(), 1);
		if (Cells.Num() == 1)
		{
			TestEqual(TEXT("Payload cell preserves i32 bits"), static_cast<int32>(Cells[0]), 42);
		}
		TestFalse(
			TEXT("Consumed result capability is stale"),
			Owner->ConsumeResult(
				EAvidScriptContinuationLane::Active,
				ActiveHost.GetActivationSerial(),
				Completion.ObjectSlot,
				Completion.ObjectGeneration,
				Provider->GetPayloadTypeId(),
				Cells));
		TestTrue(
			TEXT("Consumed provider completion finalizes"),
			Owner->FinalizeDispatched(Completion.Token, true));
	}
	TestEqual(TEXT("Consumed result slot is reclaimed"), Owner->GetResultSlotCountForTesting(), 0);

	FAvidScriptBindingLatentReservation Cancelled;
	TestTrue(
		TEXT("Cancellable provider latent reserves"),
		ActiveHost.BeginLatentWithCompletion(402, Contract, Cancelled));
	Provider->Publish(Cancelled.CallbackTarget, Cancelled.UUID, 7);
	RegisterTestLatentAction(World, Cancelled);
	TestTrue(TEXT("Cancellable provider latent commits"), ActiveHost.CommitLatent(Cancelled.Token));
	TestTrue(TEXT("Cancellation wins before provider completion"), ActiveHost.Cancel(Cancelled.Token));
	TestEqual(TEXT("Cancellation abandons provider state"), Provider->AbandonCount, 1);
	TestEqual(TEXT("Cancellation leaves no result slot"), Owner->GetResultSlotCountForTesting(), 0);

	FAvidScriptContinuationHostEndpoint& PreparedHost =
		Owner->BeginPrepared(World);
	FAvidScriptBindingLatentReservation Prepared;
	TestTrue(
		TEXT("Prepared provider latent reserves"),
		PreparedHost.BeginLatentWithCompletion(403, Contract, Prepared));
	Provider->Publish(Prepared.CallbackTarget, Prepared.UUID, 99);
	TestTrue(
		TEXT("Prepared provider can complete before commit"),
		TriggerLatentReservationSynchronously(Prepared));
	TestTrue(TEXT("Prepared provider latent commits"), PreparedHost.CommitLatent(Prepared.Token));
	TestEqual(TEXT("Prepared completion owns one result slot"), Owner->GetResultSlotCountForTesting(), 1);
	FString CommitError;
	TestTrue(TEXT("Prepared provider lane validates"), Owner->ValidatePreparedCommit(CommitError));
	Owner->CommitPrepared();
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Promoted provider completion drains"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TArray<uint64> Cells;
		TestTrue(
			TEXT("Promoted result consumes on the active lane"),
			Owner->ConsumeResult(
				EAvidScriptContinuationLane::Active,
				PreparedHost.GetActivationSerial(),
				Completions[0].ObjectSlot,
				Completions[0].ObjectGeneration,
				Provider->GetPayloadTypeId(),
				Cells));
		TestTrue(
			TEXT("Promoted provider completion finalizes"),
			Owner->FinalizeDispatched(Completions[0].Token, true));
	}
	TestEqual(TEXT("Promoted result slot is reclaimed"), Owner->GetResultSlotCountForTesting(), 0);

	FAvidScriptContinuationHostEndpoint& DiscardedHost =
		Owner->BeginPrepared(World);
	FAvidScriptBindingLatentReservation Discarded;
	TestTrue(
		TEXT("Discarded provider latent reserves"),
		DiscardedHost.BeginLatentWithCompletion(404, Contract, Discarded));
	Provider->Publish(Discarded.CallbackTarget, Discarded.UUID, 5);
	TestTrue(
		TEXT("Discarded provider completes before commit"),
		TriggerLatentReservationSynchronously(Discarded));
	TestTrue(TEXT("Discarded provider latent commits"), DiscardedHost.CommitLatent(Discarded.Token));
	TestEqual(TEXT("Discard candidate owns a result slot"), Owner->GetResultSlotCountForTesting(), 1);
	Owner->DiscardPrepared();
	TestEqual(TEXT("Discard reclaims its result slot"), Owner->GetResultSlotCountForTesting(), 0);

	Owner->Teardown();
	TestEqual(TEXT("Teardown leaves no result slots"), Owner->GetResultSlotCountForTesting(), 0);
	DestroyContinuationWorld(World);
	return true;
}

#endif
