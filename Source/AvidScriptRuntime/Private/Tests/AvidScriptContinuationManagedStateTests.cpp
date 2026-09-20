#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/Engine.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include <array>

namespace AvidScriptContinuationManagedStateTests
{
class FPendingCancellationAction final : public FPendingLatentAction
{
public:
	void UpdateOperation(FLatentResponse&) override {}
};

class FCancelledProvider final : public IAvidScriptLatentCompletionProvider
{
public:
	FString GetProviderId() const override { return TEXT("managed-state-cancel"); }
	FString GetFunctionPath() const override { return TEXT("/Script/AvidScriptTest.ManagedStateCancel"); }
	FString GetPayloadTypeId() const override { return TEXT("int32"); }
	bool ConsumePayload(UObject*, int32, FAvidScriptBindingLatentCompletionPayload&) override { return false; }
	void AbandonPayload(UObject*, int32) override {}
};

struct FWorldFixture
{
	UWorld* World = nullptr;
	FWorldFixture()
	{
		if (!GEngine) return;
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptManagedContinuationWorld"));
		if (!World) return;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
	}
	~FWorldFixture()
	{
		if (!World) return;
		if (GEngine) GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	void Advance()
	{
		World->Tick(LEVELTICK_All, 0.0f);
		++GFrameCounter;
		World->Tick(LEVELTICK_All, 0.02f);
		++GFrameCounter;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptContinuationManagedStateTest,
	"AvidScript.Runtime.Continuation.ManagedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationManagedStateTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	using namespace AvidScriptContinuationManagedStateTests;
	FWorldFixture Fixture;
	if (!TestNotNull(TEXT("Managed continuation World exists"), Fixture.World)) return false;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection), Other(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(TEXT("Runtime loads"), Runtime.LoadEmbeddedSmokeModule(Result))
			|| !TestTrue(TEXT("Independent runtime loads"), Other.LoadEmbeddedSmokeModule(Result))) return false;
		FHeap& Heap = *Runtime.GetManagedHeapForTesting();
		const std::array<FHeapLayout, 1> Layouts{{{1, 8, {{0, 1}}}}};
		if (!TestTrue(TEXT("Cyclic state layout configures"), Heap.Configure(Layouts) == EHeapError::Ok)) return false;
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		IAvidScriptContinuationHost* Host = &Owner->ResetActive(Fixture.World);
		const uint8 State[] = {1, 2, 3, 4};
		TArray<FAvidScriptContinuationCompletion> Ready;
		auto Acquire = [&](TUniquePtr<IAvidScriptContinuationStateLease>& Lease)
		{
			FToken Frame = 0, Root = 0, Object = 0;
			TestTrue(TEXT("Temporary frame"), Heap.PushFrame(Frame) == EHeapError::Ok);
			TestTrue(TEXT("Temporary root"), Heap.CreateRoot(Frame, 0, Root) == EHeapError::Ok);
			TestTrue(TEXT("State object allocation"), Heap.Allocate(1, Root, Object) == EHeapError::Ok);
			TestTrue(TEXT("State object cycle"), Heap.WriteReference(Object, 1, 0, Object) == EHeapError::Ok);
			const uint64 Objects[] = {Object};
			TestTrue(TEXT("Native state lease acquired"), Runtime.CreateContinuationStateLease(MakeArrayView(Objects), Lease));
			TestTrue(TEXT("Temporary frame exits"), Heap.PopFrame(Frame) == EHeapError::Ok);
			return Object;
		};
		auto CollectAlive = [&](FToken Object, bool Expected)
		{
			TestTrue(TEXT("Collect at ownership boundary"), Heap.Collect() == EHeapError::Ok);
			TestEqual(TEXT("State liveness matches owner"), Heap.IsAlive(Object), Expected);
		};
		auto Attach = [&](IAvidScriptContinuationHost& Endpoint, int64 Token)
		{
			TUniquePtr<IAvidScriptContinuationStateLease> Lease;
			const FToken Object = Acquire(Lease);
			TestTrue(TEXT("Managed state attaches"), Endpoint.StoreManagedState(Token, Runtime, MakeArrayView(State), MoveTemp(Lease)));
			TestFalse(TEXT("Successful store transfers ownership"), Lease.IsValid());
			CollectAlive(Object, true);
			return Object;
		};
		for (const bool Success : {true, false})
		{
			const int64 Token = Host->ScheduleDelay(0.01f, 51);
			const FToken Object = Attach(*Host, Token);
			uint8 Output[4] = {9, 9, 9, 9};
			TestFalse(TEXT("Pending state cannot be consumed"), Host->ReadManagedState(Token, Runtime, MakeArrayView(Output)));
			Fixture.Advance(); Owner->DrainReady(Ready);
			if (!TestEqual(TEXT("One real Timer continuation dispatches"), Ready.Num(), 1)) return false;
			TestFalse(TEXT("Legacy read cannot bypass lease"), Host->ReadState(Token, MakeArrayView(Output)));
			TestFalse(TEXT("Other heap cannot read state"), Host->ReadManagedState(Token, Other, MakeArrayView(Output)));
			TestEqual(TEXT("Rejected reads leave output untouched"), Output[0], uint8(9));
			TestTrue(TEXT("Matching heap reads state"), Host->ReadManagedState(Token, Runtime, MakeArrayView(Output)));
			TestTrue(TEXT("State bytes preserved"), FMemory::Memcmp(Output, State, sizeof(State)) == 0);
			TestFalse(TEXT("State consumed once"), Host->ReadManagedState(Token, Runtime, MakeArrayView(Output)));
			CollectAlive(Object, true);
			TestTrue(TEXT("Dispatch finalizes after Guest return or failure"), Owner->FinalizeDispatched(Token, Success));
			CollectAlive(Object, false);
		}

		// Failed acquisition/store must preserve caller ownership and published bytes.
		TUniquePtr<IAvidScriptContinuationStateLease> Lease;
		const FToken Retained = Acquire(Lease);
		const auto* OriginalLease = Lease.Get();
		const uint64 InvalidObjects[] = {Retained, ~uint64(0)};
		TestFalse(TEXT("Partial acquisition rejects invalid object"), Runtime.CreateContinuationStateLease(MakeArrayView(InvalidObjects), Lease));
		TestTrue(TEXT("Failed acquisition preserves old lease"), Lease.Get() == OriginalLease);
		const int64 RejectedToken = Host->ScheduleDelay(10.0f, 52);
		TArray<uint8> Oversized;
		Oversized.SetNumZeroed(FAvidScriptSessionContinuations::MaximumStateFrameBytes + 1);
		TestFalse(TEXT("Oversized state rejected before transfer"), Host->StoreManagedState(RejectedToken, Runtime, Oversized, MoveTemp(Lease)));
		TestFalse(TEXT("Stale token rejected before transfer"), Host->StoreManagedState(0, Runtime, MakeArrayView(State), MoveTemp(Lease)));
		TestTrue(TEXT("Rejected shape and token preserve ownership"), Lease.Get() == OriginalLease);
		TestFalse(TEXT("Wrong runtime rejected before transfer"), Host->StoreManagedState(RejectedToken, Other, MakeArrayView(State), MoveTemp(Lease)));
		TestTrue(TEXT("Wrong runtime keeps caller lease"), Lease.Get() == OriginalLease);
		TestTrue(TEXT("Legacy state attaches"), Host->StoreState(RejectedToken, MakeArrayView(State)));
		TestFalse(TEXT("Duplicate state rejected before transfer"), Host->StoreManagedState(RejectedToken, Runtime, MakeArrayView(State), MoveTemp(Lease)));
		TestTrue(TEXT("Duplicate keeps caller lease"), Lease.Get() == OriginalLease);
		TestTrue(TEXT("Legacy state cancels"), Host->Cancel(RejectedToken));
		CollectAlive(Retained, true);
		Lease.Reset(); CollectAlive(Retained, false);

		for (const bool ReadyBeforeCancel : {false, true})
		{
			const int64 Token = Host->ScheduleDelay(0.01f, 53);
			const FToken Object = Attach(*Host, Token);
			if (ReadyBeforeCancel) Fixture.Advance();
			TestTrue(TEXT("Pending or ready Timer cancels"), Host->Cancel(Token));
			TestFalse(TEXT("Repeated cancellation is stale"), Host->Cancel(Token));
			CollectAlive(Object, false);
		}
		const int64 Source = Host->CreateCancellationSource();
		const int64 Bound = Host->ScheduleDelay(10.0f, 54);
		const FToken BoundObject = Attach(*Host, Bound);
		TestTrue(TEXT("Bind cancellation source"), Host->BindCancellationSource(Source, Bound));
		TestTrue(TEXT("Source cancellation releases state"), Host->CancelCancellationSource(Source));
		CollectAlive(BoundObject, false);
		Host->ReleaseCancellationSource(Source);

		// Cancellation with a terminal outcome still needs the state for its callback.
		FAvidScriptBindingLatentCompletionContract Contract;
		Contract.Provider = MakeShared<FCancelledProvider>();
		Contract.Mode = TEXT("provider");
		Contract.ProviderId = Contract.Provider->GetProviderId();
		Contract.PayloadTypeId = Contract.Provider->GetPayloadTypeId();
		Contract.StatusPolicy = TEXT("resume_outcome_on_cancel");
		Contract.bCancellable = true;
		FAvidScriptBindingLatentReservation Reservation;
		auto& Endpoint = static_cast<FAvidScriptContinuationHostEndpoint&>(*Host);
		if (!TestTrue(TEXT("Terminal latent reservation"), Endpoint.BeginLatentWithCompletion(60, Contract, Reservation))) return false;
		const FToken TerminalObject = Attach(*Host, Reservation.Token);
		Fixture.World->GetLatentActionManager().AddNewAction(
			Reservation.CallbackTarget, Reservation.UUID, new FPendingCancellationAction());
		TestTrue(TEXT("Terminal latent commits"), Endpoint.CommitLatent(Reservation.Token));
		TestTrue(TEXT("Terminal cancellation queues result"), Host->Cancel(Reservation.Token));
		CollectAlive(TerminalObject, true);
		Owner->DrainReady(Ready);
		if (!TestEqual(TEXT("Cancellation outcome dispatches once"), Ready.Num(), 1)) return false;
		TestTrue(TEXT("Terminal status is cancelled"), Ready[0].Status == EAvidScriptContinuationStatus::Cancelled);
		uint8 TerminalOutput[4] = {};
		TestTrue(TEXT("Cancelled callback reads rooted state"), Host->ReadManagedState(Reservation.Token, Runtime, MakeArrayView(TerminalOutput)));
		CollectAlive(TerminalObject, true);
		TestTrue(TEXT("Cancelled callback finalizes"), Owner->FinalizeDispatched(Reservation.Token, true));
		CollectAlive(TerminalObject, false);

		const int64 ActiveToken = Host->ScheduleDelay(10.0f, 55);
		const FToken ActiveObject = Attach(*Host, ActiveToken);
		IAvidScriptContinuationHost& Discarded = Owner->BeginPrepared(Fixture.World);
		const FToken DiscardedObject = Attach(Discarded, Discarded.ScheduleDelay(10.0f, 56));
		Owner->DiscardPrepared();
		CollectAlive(DiscardedObject, false); CollectAlive(ActiveObject, true);
		IAvidScriptContinuationHost& Promoted = Owner->BeginPrepared(Fixture.World);
		const FToken PromotedObject = Attach(Promoted, Promoted.ScheduleDelay(10.0f, 57));
		FString CommitError;
		TestTrue(TEXT("Prepared commit validates"), Owner->ValidatePreparedCommit(CommitError));
		Owner->CommitPrepared();
		CollectAlive(ActiveObject, false); CollectAlive(PromotedObject, true);
		Owner->ReleaseRetiredEndpoint();
		Host = &Promoted;
		Owner->Teardown(); Owner->Teardown();
		CollectAlive(PromotedObject, false);

		Host = &Owner->ResetActive(Fixture.World);
		const FToken WorldObject = Attach(*Host, Host->ScheduleDelay(0.01f, 58));
		Fixture.World->bIsTearingDown = true;
		Owner->DrainReady(Ready);
		Fixture.World->bIsTearingDown = false;
		CollectAlive(WorldObject, false);

		Host = &Owner->ResetActive(Fixture.World);
		const int64 OldToken = Host->ScheduleDelay(0.01f, 59);
		Attach(*Host, OldToken);
		Fixture.Advance(); Owner->DrainReady(Ready);
		TestEqual(TEXT("Old state starts dispatch"), Ready.Num(), 1);
		Runtime.Unload();
		uint8 Output[4] = {9, 9, 9, 9};
		TestFalse(TEXT("Unloaded runtime cannot resume"), Host->ReadManagedState(OldToken, Runtime, MakeArrayView(Output)));
		if (!TestTrue(TEXT("Same runtime loads new code"), Runtime.LoadEmbeddedSmokeModule(Result))) return false;
		TestFalse(TEXT("Reloaded runtime cannot revive old lease"), Host->ReadManagedState(OldToken, Runtime, MakeArrayView(Output)));
		TestEqual(TEXT("Old state rejection leaves output untouched"), Output[0], uint8(9));
		TestTrue(TEXT("Late finalization safely releases dead heap lease"), Owner->FinalizeDispatched(OldToken, false));
		TestEqual(TEXT("All state bytes released"), Owner->GetStateFrameByteCountForTesting(), 0);
		TestEqual(TEXT("Replacement heap has no roots"), Runtime.GetManagedHeapForTesting()->GetStats().LiveRoots, uint32(0));
		Owner->Teardown();
	}
	return true;
}
#endif
