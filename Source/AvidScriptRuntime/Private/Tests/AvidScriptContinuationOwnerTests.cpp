#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptAsyncObjectLoader.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "AvidScriptWasmRuntime.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"

#include "Containers/StringConv.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

namespace
{
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

	static constexpr int64 ExpectedToken = 0x000000010000002aLL;
	float LastDelaySeconds = 0.0f;
	FString LastObjectPath;
	int32 LastCallbackId = 0;
	int64 LastCancelledToken = 0;
	int32 ScheduleCount = 0;
	int32 ObjectLoadScheduleCount = 0;
	int32 CancelCount = 0;
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
	const TSharedPtr<FAvidScriptFakeAsyncObjectLoader> Loader =
		MakeShared<FAvidScriptFakeAsyncObjectLoader>();
	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>(Loader);
	FAvidScriptObjectRegistry Registry;
	FAvidScriptSessionObjectOwnership Ownership;
	IAvidScriptContinuationHost& InitialHost = Owner->ResetActive(
		nullptr,
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
		nullptr,
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
		nullptr,
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
	return true;
}

#endif
