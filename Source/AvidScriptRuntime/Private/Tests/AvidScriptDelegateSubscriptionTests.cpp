#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeSession.h"
#include "Delegate/AvidScriptDelegateBridge.h"
#include "Session/AvidScriptSessionDelegateSubscriptions.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Tests/AvidScriptDelegateSubscriptionTestTypes.h"
#include <array>
#include "Engine/World.h"

#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
bool EncodeSourceContextTestFrame(
	const void* CodecIdentity, const void*, const FAvidScriptBindingInvocationContext&,
	uint32, FAvidScriptVmCallFrame& OutFrame, TArray<FAvidScriptObjectHandle>&,
	FString&, FString&)
{
	(*static_cast<const TFunction<void()>*>(CodecIdentity))();
	const float Value = 0.016f;
	OutFrame.CellCount = 1;
	FMemory::Memcpy(&OutFrame.Cells[0], &Value, sizeof(Value));
	return true;
}

bool EncodeDelegateTestFrame(
	const void* CodecIdentity,
	const void* NativeParameters,
	const FAvidScriptBindingInvocationContext&,
	uint32,
	FAvidScriptVmCallFrame& OutFrame,
	TArray<FAvidScriptObjectHandle>&,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	const UFunction* const SignatureFunction =
		static_cast<const UFunction*>(CodecIdentity);
	const FFloatProperty* const FloatProperty = SignatureFunction != nullptr
		? FindFProperty<FFloatProperty>(SignatureFunction, TEXT("FloatValue"))
		: nullptr;
	if (FloatProperty == nullptr || NativeParameters == nullptr)
	{
		OutErrorCategory = TEXT("delegate_test_parameter_missing");
		OutErrorDetails = TEXT("The real multicast fixture did not expose FloatValue.");
		return false;
	}
	const float Value = FloatProperty->GetPropertyValue_InContainer(NativeParameters);
	OutFrame.CellCount = 1;
	FMemory::Memcpy(&OutFrame.Cells[0], &Value, sizeof(Value));
	return true;
}

bool EncodeSinglecastTestFrame(
	const void* CodecIdentity,
	const void* NativeParameters,
	const FAvidScriptBindingInvocationContext&,
	uint32,
	FAvidScriptVmCallFrame& OutFrame,
	TArray<FAvidScriptObjectHandle>&,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	const UFunction* const SignatureFunction =
		static_cast<const UFunction*>(CodecIdentity);
	const FIntProperty* const IntProperty = SignatureFunction != nullptr
		? FindFProperty<FIntProperty>(SignatureFunction, TEXT("IntValue"))
		: nullptr;
	if (IntProperty == nullptr || NativeParameters == nullptr)
	{
		OutErrorCategory = TEXT("delegate_test_parameter_missing");
		OutErrorDetails = TEXT("The real singlecast fixture did not expose IntValue.");
		return false;
	}
	const int32 Value = IntProperty->GetPropertyValue_InContainer(NativeParameters);
	OutFrame.CellCount = 1;
	FMemory::Memcpy(&OutFrame.Cells[0], &Value, sizeof(Value));
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptDelegateCurrentSourceTest,
	"AvidScript.Runtime.DelegateSubscription.CurrentSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptDelegateCurrentSourceTest::RunTest(const FString& Parameters)
{
	auto* Source = NewObject<UAvidScriptRuntimeDelegateTestObject>();
	auto* Other = NewObject<UAvidScriptRuntimeDelegateTestObject>();
	auto* Property = FindFProperty<FMulticastDelegateProperty>(Source->GetClass(),
		GET_MEMBER_NAME_CHECKED(UAvidScriptRuntimeDelegateTestObject, OnSignal));
	if (!TestNotNull(TEXT("Fixture has a real multicast property"), Property))
	{
		return false;
	}
	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult LoadResult;
	if (!TestTrue(TEXT("Context fixture loads a real Guest"), Session.LoadEmbeddedSmoke(LoadResult)))
	{
		return false;
	}
	FAvidScriptSessionDelegateSubscriptions Subscriptions(Session);
	TFunction<void()> Observe;
	FAvidScriptPreparedDelegateEvent Event;
	Event.EventOrdinal = 0;
	Event.StableId = FString::ChrN(64, TEXT('e'));
	Event.ExportName = TEXT("avid_on_tick");
	Event.ExpectedSourceClass = Source->GetClass();
	Event.Signature.Kind = EAvidScriptPreparedDelegateKind::Multicast;
	Event.Signature.MulticastProperty = Property;
	Event.Signature.SignatureFunction = Property->SignatureFunction;
	Event.Signature.ParameterCellCount = 1;
	Event.Signature.ImmutableCodecIdentity = &Observe;
	Event.Signature.Encode = &EncodeSourceContextTestFrame;
	FString Error;
	if (!TestTrue(TEXT("Source context subscription prepares"),
		Subscriptions.Prepare(Source, MakeArrayView(&Event, 1), Error)))
	{
		return false;
	}
	Subscriptions.CommitPrepared();
	Subscriptions.SetDispatchEnabled(true);
	const int64 OtherToken = Subscriptions.Subscribe(*Other, 0, Error);
	TestTrue(TEXT("Second source shares the event contract"), OtherToken > 0);
	TestFalse(TEXT("Source is absent outside callback"), Subscriptions.IsCurrentSource(*Source));
	Observe = [&]()
	{
		TestTrue(TEXT("Callback sees its actual source"), Subscriptions.IsCurrentSource(*Source));
		TestFalse(TEXT("Another source does not match"), Subscriptions.IsCurrentSource(*Other));
		Other->Broadcast(Other, 1, 1.0f);
		TestTrue(TEXT("Rejected reentry restores outer source"), Subscriptions.IsCurrentSource(*Source));
		TestFalse(TEXT("Rejected reentry does not leak nested source"), Subscriptions.IsCurrentSource(*Other));
	};
	Source->Broadcast(Source, 1, 1.0f);
	TestEqual(TEXT("Reentry did not add a Guest callback"), Session.GetLiveEventCallbackCount(), 1);
	TestFalse(TEXT("Normal return clears source"), Subscriptions.IsCurrentSource(*Source));
	Observe = [&]()
	{
		TestTrue(TEXT("Second instance has its own source"), Subscriptions.IsCurrentSource(*Other));
		TestTrue(TEXT("Callback can cancel its own entry"), Subscriptions.Unsubscribe(OtherToken, Error));
		TestTrue(TEXT("Entry removal preserves the executing callback identity"), Subscriptions.IsCurrentSource(*Other));
		Subscriptions.UnbindActive();
		TestFalse(TEXT("Teardown immediately invalidates callback context"), Subscriptions.IsCurrentSource(*Other));
	};
	Other->Broadcast(Other, 2, 2.0f);
	TestEqual(TEXT("Second source reaches the Guest once"), Session.GetLiveEventCallbackCount(), 2);
	TestFalse(TEXT("Teardown return leaves no context"), Subscriptions.IsCurrentSource(*Other));
	Source->Broadcast(Source, 3, 3.0f);
	TestEqual(TEXT("Teardown prevents later delivery"), Session.GetLiveEventCallbackCount(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptDelegateBridgeLifecycleTest,
	"AvidScript.Runtime.DelegateSubscription.BridgeLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptDelegateManagedStateTest,
	"AvidScript.Runtime.DelegateSubscription.ManagedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptDelegateManagedStateTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (int32 Scenario = 0; Scenario < 12; ++Scenario)
	{
		AddInfo(FString::Printf(TEXT("Managed subscription backend=%d scenario=%d"), static_cast<int32>(Backend), Scenario));
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptRuntimeSession Session;
		Session.SetBackendSelectionForTesting(Selection);
		FAvidScriptWasmReloadResult Loaded;
		if (!Session.LoadEmbeddedSmoke(Loaded)) { AddError(Loaded.ErrorMessage); return false; }
		auto* Runtime = Session.GetLiveRuntimeForTesting();
		FAvidScriptWasmRuntimeInstance Other(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!Other.LoadEmbeddedSmokeModule(Result)) { AddError(Result.ErrorMessage); return false; }
		auto& Heap = *Runtime->GetManagedHeapForTesting();
		auto& OtherHeap = *Other.GetManagedHeapForTesting();
		const std::array<FHeapLayout, 1> Layouts{{{1, 8, {{0, 1}}}}};
		if (!TestTrue(TEXT("subscription state layout configures"), Heap.Configure(Layouts) == EHeapError::Ok)
			|| !TestTrue(TEXT("foreign state layout configures"), OtherHeap.Configure(Layouts) == EHeapError::Ok)) return false;
		TStrongObjectPtr<UWorld> CatalogSource(NewObject<UWorld>());
		TStrongObjectPtr<UAvidScriptRuntimeDelegateTestObject> Source(NewObject<UAvidScriptRuntimeDelegateTestObject>());
		TStrongObjectPtr<UAvidScriptRuntimeDelegateTestObject> Peer(NewObject<UAvidScriptRuntimeDelegateTestObject>());
		FAvidScriptSessionDelegateSubscriptions Subscriptions(Session);
		TFunction<void()> Observe = []() {};
		FAvidScriptPreparedDelegateEvent Event;
		Event.EventOrdinal = 0;
		// Bridge function identity uses the first 16 hex characters.
		Event.StableId = (Scenario == 10 ? FString(TEXT("b713b713b713b713")) : FString(TEXT("c813c813c813c813"))) + FString::ChrN(48, 'c');
		Event.ExportName = TEXT("avid_on_tick");
		Event.ExpectedSourceClass = Source->GetClass();
		if (Scenario == 10)
		{
			Event.Signature.Kind = EAvidScriptPreparedDelegateKind::Singlecast;
			Event.Signature.SinglecastProperty = FindFProperty<FDelegateProperty>(Source->GetClass(), TEXT("OnSinglecast"));
			Event.Signature.SignatureFunction = Event.Signature.SinglecastProperty->SignatureFunction;
		}
		else
		{
			Event.Signature.Kind = EAvidScriptPreparedDelegateKind::Multicast;
			Event.Signature.MulticastProperty = FindFProperty<FMulticastDelegateProperty>(Source->GetClass(), TEXT("OnSignal"));
			Event.Signature.SignatureFunction = Event.Signature.MulticastProperty->SignatureFunction;
		}
		Event.Signature.ParameterCellCount = 1;
		Event.Signature.ImmutableCodecIdentity = &Observe;
		Event.Signature.Encode = &EncodeSourceContextTestFrame;
		FString Error;
		if (!Subscriptions.Prepare(CatalogSource.Get(), MakeArrayView(&Event, 1), Error, Runtime)) { AddError(Error); return false; }
		const bool bPrepared = Scenario == 5 || Scenario == 6 || Scenario == 9;
		if (!bPrepared) { Subscriptions.CommitPrepared(); Subscriptions.SetDispatchEnabled(true); }
		auto Acquire = [&](FAvidScriptWasmRuntimeInstance& Owner, TUniquePtr<IAvidScriptManagedStateLease>& Lease)
		{
			auto& OwnerHeap = *Owner.GetManagedHeapForTesting();
			FToken Frame = 0, Root = 0, Object = 0;
			TestTrue(TEXT("state frame opens"), OwnerHeap.PushFrame(Frame) == EHeapError::Ok);
			TestTrue(TEXT("state temporary root opens"), OwnerHeap.CreateRoot(Frame, 0, Root) == EHeapError::Ok);
			TestTrue(TEXT("state object allocates"), OwnerHeap.Allocate(1, Root, Object) == EHeapError::Ok);
			TestTrue(TEXT("state graph contains a cycle"), OwnerHeap.WriteReference(Object, 1, 0, Object) == EHeapError::Ok);
			const uint64 Objects[] = {Object};
			TestTrue(TEXT("state persistent lease acquired"), Owner.CreateManagedStateLease(MakeArrayView(Objects), Lease));
			TestTrue(TEXT("temporary frame exits"), OwnerHeap.PopFrame(Frame) == EHeapError::Ok);
			return Object;
		};
		auto Bytes = [](const FToken& Object) { return MakeArrayView(reinterpret_cast<const uint8*>(&Object), sizeof(Object)); };
		auto Collect = [&](FToken Object, bool bExpected)
		{
			TestTrue(TEXT("collect subscription graph"), Heap.Collect() == EHeapError::Ok);
			TestEqual(TEXT("subscription graph liveness"), Heap.IsAlive(Object), bExpected);
			TestEqual(TEXT("subscription leaves no executing frame"), Heap.GetStats().ActiveFrames, 0u);
		};
		TUniquePtr<IAvidScriptManagedStateLease> Lease;
		const FToken Object = Acquire(*Runtime, Lease);
		const auto* Identity = Lease.Get();
		TUniquePtr<IAvidScriptManagedStateLease> ForeignLease;
		const FToken ForeignObject = Acquire(Other, ForeignLease);
		TestEqual(TEXT("matching catalog rejects a foreign heap lease"), Subscriptions.SubscribeManaged(*Source, 0, *Runtime, Bytes(ForeignObject), MoveTemp(ForeignLease), Error), int64(0));
		TestTrue(TEXT("foreign lease remains with caller"), ForeignLease.IsValid());
		ForeignLease.Reset();
		TestEqual(TEXT("foreign Runtime cannot publish state"), Subscriptions.SubscribeManaged(*Source, 0, Other, Bytes(Object), MoveTemp(Lease), Error), int64(0));
		TestTrue(TEXT("failure preserves caller state lease"), Lease.Get() == Identity);
		TestEqual(TEXT("empty state cannot publish"), Subscriptions.SubscribeManaged(*Source, 0, *Runtime, {}, MoveTemp(Lease), Error), int64(0));
		TArray<uint8> Oversized; Oversized.SetNumZeroed(64 * 1024 + 1);
		TestEqual(TEXT("bounded state cannot overflow"), Subscriptions.SubscribeManaged(*Source, 0, *Runtime, Oversized, MoveTemp(Lease), Error), int64(0));
		TestEqual(TEXT("missing event cannot consume state"), Subscriptions.SubscribeManaged(*Source, 99, *Runtime, Bytes(Object), MoveTemp(Lease), Error), int64(0));
		TestEqual(TEXT("wrong source class cannot consume state"), Subscriptions.SubscribeManaged(*CatalogSource, 0, *Runtime, Bytes(Object), MoveTemp(Lease), Error), int64(0));
		TestTrue(TEXT("all publication failures preserve original lease"), Lease.Get() == Identity);
		const int64 Token = Subscriptions.SubscribeManaged(*Source, 0, *Runtime, Bytes(Object), MoveTemp(Lease), Error);
		if (!TestTrue(TEXT("managed subscription publishes atomically"), Token > 0 && !Lease)) { AddError(Error); return false; }
		Collect(Object, true);
		uint64 Outside = 999;
		TestFalse(TEXT("state is unreadable outside callback"), Subscriptions.ReadCurrentManagedState(*Runtime, MakeArrayView(reinterpret_cast<uint8*>(&Outside), sizeof(Outside))));
		TestEqual(TEXT("rejected read preserves output"), Outside, uint64(999));
		int32 Calls = 0;
		Observe = [&]()
		{
			++Calls;
			if (Scenario == 1) TestTrue(TEXT("callback unsubscribes itself"), Subscriptions.Unsubscribe(Token, Error));
			if (Scenario == 3) Peer->Broadcast(Peer.Get(), 1, 1.0f);
			Collect(Object, true);
			uint64 Read = 999; auto Output = MakeArrayView(reinterpret_cast<uint8*>(&Read), sizeof(Read));
			TestFalse(TEXT("foreign heap cannot read current state"), Subscriptions.ReadCurrentManagedState(Other, Output));
			uint8 Short[1] = {9};
			TestFalse(TEXT("wrong state size rejected"), Subscriptions.ReadCurrentManagedState(*Runtime, MakeArrayView(Short)));
			TestEqual(TEXT("wrong size leaves bytes untouched"), Short[0], uint8(9));
			if (Scenario == 2)
			{
				Subscriptions.UnbindActive();
				TestFalse(TEXT("teardown revokes an unread callback state"), Subscriptions.ReadCurrentManagedState(*Runtime, Output));
				TestEqual(TEXT("revoked read preserves output"), Read, uint64(999));
				Collect(Object, true);
				return;
			}
			TestTrue(TEXT("current callback reads its own state"), Subscriptions.ReadCurrentManagedState(*Runtime, Output));
			TestEqual(TEXT("state object identity is preserved"), Read, Object);
			TestFalse(TEXT("callback state reads once per invocation"), Subscriptions.ReadCurrentManagedState(*Runtime, Output));
		};
		FToken PeerObject = 0;
		if (Scenario == 3 || Scenario == 10)
		{
			PeerObject = Acquire(*Runtime, Lease);
			const int64 PeerToken = Subscriptions.SubscribeManaged(Scenario == 10 ? *Source : *Peer, 0, *Runtime, Bytes(PeerObject), MoveTemp(Lease), Error);
			if (Scenario == 10)
			{
				TestEqual(TEXT("singlecast duplicate cannot replace state"), PeerToken, int64(0));
				TestTrue(TEXT("duplicate failure preserves caller lease"), Lease.IsValid());
				Lease.Reset(); Collect(PeerObject, false);
			}
			else TestTrue(TEXT("peer source has separate callback state"), PeerToken > 0);
		}
		if (Scenario == 5) { Subscriptions.DiscardPrepared(); Collect(Object, false); }
		else if (Scenario == 6) { Subscriptions.CommitPrepared(); Subscriptions.SetDispatchEnabled(true); }
		else if (Scenario == 4 || Scenario == 9)
		{
			TWeakObjectPtr<UAvidScriptRuntimeDelegateTestObject> Weak(Source.Get());
			Source.Reset(); CollectGarbage(RF_NoFlags);
			TestFalse(TEXT("subscription does not retain UObject source"), Weak.IsValid());
			TestEqual(TEXT("source GC removes active subscriptions"), Subscriptions.NumActive(), 0);
			TestEqual(TEXT("source GC removes prepared subscriptions"), Subscriptions.NumPrepared(), 0);
			Collect(Object, false);
			if (Scenario == 9) Subscriptions.CommitPrepared();
		}
		else if (Scenario == 7 || Scenario == 8)
		{
			if (!Subscriptions.Prepare(CatalogSource.Get(), MakeArrayView(&Event, 1), Error, &Other)) return false;
			PeerObject = Acquire(Other, Lease);
			if (!TestTrue(TEXT("candidate state belongs to candidate Runtime"), Subscriptions.SubscribeManaged(*Peer, 0, Other, Bytes(PeerObject), MoveTemp(Lease), Error) > 0)) return false;
			if (Scenario == 7) { Subscriptions.DiscardPrepared(); Collect(Object, true); }
			else { Subscriptions.CommitPrepared(); Collect(Object, false); }
			TestTrue(TEXT("candidate heap collects"), OtherHeap.Collect() == EHeapError::Ok);
			TestEqual(TEXT("candidate transaction owns exactly its graph"), OtherHeap.IsAlive(PeerObject), Scenario == 8);
		}
		else if (Scenario == 11)
		{
			const FToken OldObject = Acquire(*Runtime, Lease);
			if (!Session.StopAndUnload(Result) || !Session.LoadEmbeddedSmoke(Loaded)) return false;
			Subscriptions.UnbindActive();
			auto* Replacement = Session.GetLiveRuntimeForTesting();
			if (!Subscriptions.Prepare(CatalogSource.Get(), MakeArrayView(&Event, 1), Error, Replacement)) return false;
			TestEqual(TEXT("retired heap lease cannot enter replacement code"), Subscriptions.SubscribeManaged(*Source, 0, *Replacement, Bytes(OldObject), MoveTemp(Lease), Error), int64(0));
			TestTrue(TEXT("expired lease rejection preserves caller ownership"), Lease.IsValid());
			Lease.Reset(); Subscriptions.DiscardPrepared();
			TestEqual(TEXT("late release cannot alter replacement Runtime"), Session.GetLiveRuntimeForTesting()->GetManagedHeapForTesting()->GetStats().LiveRoots, 0u);
			continue;
		}
		if (Scenario <= 3 || Scenario == 6 || Scenario == 7 || Scenario == 10)
		{
			if (Scenario == 10) Source->ExecuteSinglecast(1); else Source->Broadcast(Source.Get(), 1, 1.0f);
			TestEqual(TEXT("one real UE event reached the callback"), Calls, 1);
			Collect(Object, Scenario != 1 && Scenario != 2);
			if (Scenario == 0)
			{
				Source->Broadcast(Source.Get(), 2, 2.0f);
				TestEqual(TEXT("new event gets a fresh read capability"), Calls, 2);
			}
		}
		Subscriptions.UnbindActive(); Subscriptions.DiscardPrepared();
		Collect(Object, false);
		TestEqual(TEXT("all subscription roots released"), Heap.GetStats().LiveRoots, 0u);
		TestEqual(TEXT("all subscription graphs reclaimed"), Heap.GetStats().LiveObjects, 0u);
		TestTrue(TEXT("candidate heap final collection"), OtherHeap.Collect() == EHeapError::Ok);
		TestEqual(TEXT("all candidate roots released"), OtherHeap.GetStats().LiveRoots, 0u);
		TestEqual(TEXT("all candidate graphs reclaimed"), OtherHeap.GetStats().LiveObjects, 0u);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptDelegateExportPreparationTest,
	"AvidScript.Runtime.DelegateSubscription.ExportPreparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSinglecastDelegateLeaseTest,
	"AvidScript.Runtime.DelegateSubscription.SinglecastLease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptDelegateBridgeLifecycleTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UAvidScriptRuntimeDelegateTestObject> Source(
		NewObject<UAvidScriptRuntimeDelegateTestObject>());
	FMulticastDelegateProperty* const Property =
		FindFProperty<FMulticastDelegateProperty>(
			Source->GetClass(),
			GET_MEMBER_NAME_CHECKED(
				UAvidScriptRuntimeDelegateTestObject,
				OnSignal));
	TestNotNull(TEXT("Fixture exposes a multicast delegate property"), Property);
	if (Property == nullptr || Property->SignatureFunction == nullptr)
	{
		return false;
	}

	FAvidScriptPreparedDelegateEvent Event;
	Event.EventOrdinal = 0;
	Event.StableId = FString::ChrN(64, TEXT('a'));
	Event.ExportName = TEXT("avid_on_tick");
	Event.ExpectedSourceClass = Source->GetClass();
	Event.Signature.Kind = EAvidScriptPreparedDelegateKind::Multicast;
	Event.Signature.MulticastProperty = Property;
	Event.Signature.SignatureFunction = Property->SignatureFunction;
	Event.Signature.ParameterCellCount = 1;
	Event.Signature.ImmutableCodecIdentity = Property->SignatureFunction;
	Event.Signature.Encode = &EncodeDelegateTestFrame;

	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult LoadResult;
	if (!TestTrue(
			TEXT("Embedded runtime reaches Running before subscription"),
			Session.LoadEmbeddedSmoke(LoadResult)))
	{
		return false;
	}
	FString Error;
	TestTrue(
		TEXT("Prepared subscription accepts a compatible source"),
		Session.PrepareDelegateSubscriptionsForTesting(
			Source.Get(),
			MakeArrayView(&Event, 1),
			Error));
	Session.CommitDelegateSubscriptionsForTesting();
	TestEqual(
		TEXT("Commit activates one subscription"),
		Session.GetDelegateSubscriptionCountForTesting(),
		1);
	const FName BridgeFunctionName(
		*FString::Printf(
			TEXT("AvidDelegate_%s"),
			*Event.StableId.Left(16)));
	UFunction* const BridgeFunctionBeforeGc =
		UAvidScriptDelegateBridge::StaticClass()->FindFunctionByName(
			BridgeFunctionName,
			EIncludeSuperFlag::ExcludeSuper);
	TestNotNull(
		TEXT("Prepared subscription installs its dynamic bridge function"),
		BridgeFunctionBeforeGc);
	if (BridgeFunctionBeforeGc == nullptr)
	{
		return false;
	}
	TestTrue(
		TEXT("Permanent bridge class only references a rooted dynamic function"),
		BridgeFunctionBeforeGc->IsRooted());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestEqual(
		TEXT("Dynamic bridge function survives a verifying GC"),
		UAvidScriptDelegateBridge::StaticClass()->FindFunctionByName(
			BridgeFunctionName,
			EIncludeSuperFlag::ExcludeSuper),
		BridgeFunctionBeforeGc);

	Source->Broadcast(Source.Get(), 17, 2.5f);
	TestEqual(
		TEXT("Real UE multicast broadcast invokes one prepared guest export"),
		Session.GetLiveEventCallbackCount(),
		1);

	UObject* const InvalidSource = NewObject<UAvidScriptDelegateBridge>();
	TestTrue(
		TEXT("An incompatible self source still prepares the arbitrary-source catalog"),
		Session.PrepareDelegateSubscriptionsForTesting(
			InvalidSource,
			MakeArrayView(&Event, 1),
			Error));
	TestEqual(
		TEXT("Catalog preparation does not mutate the active subscription"),
		Session.GetDelegateSubscriptionCountForTesting(),
		1);
	Source->Broadcast(Source.Get(), 18, 2.75f);
	TestEqual(
		TEXT("The preserved active subscription still reaches the guest"),
		Session.GetLiveEventCallbackCount(),
		2);

	UAvidScriptRuntimeDelegateTestObject* const ReplacementSource =
		NewObject<UAvidScriptRuntimeDelegateTestObject>();
	Error.Reset();
	const bool bPreparedReplacement =
		Session.PrepareDelegateSubscriptionsForTesting(
			ReplacementSource,
			MakeArrayView(&Event, 1),
			Error);
	TestTrue(
		*FString::Printf(
			TEXT("A compatible replacement source prepares (error=%s)"),
			Error.IsEmpty() ? TEXT("<none>") : *Error),
		bPreparedReplacement);
	Session.CommitDelegateSubscriptionsForTesting();
	Source->Broadcast(Source.Get(), 19, 3.0f);
	TestEqual(
		TEXT("Commit removes the previous source subscription"),
		Session.GetLiveEventCallbackCount(),
		2);
	ReplacementSource->Broadcast(ReplacementSource, 20, 3.25f);
	TestEqual(
		TEXT("Commit activates the replacement source subscription"),
		Session.GetLiveEventCallbackCount(),
		3);
	Error.Reset();
	TestFalse(
		TEXT("A null prepared source is rejected without leaving prepared mode active"),
		Session.PrepareDelegateSubscriptionsForTesting(
			nullptr,
			MakeArrayView(&Event, 1),
			Error));
	ReplacementSource->Broadcast(ReplacementSource, 20, 3.5f);
	TestEqual(
		TEXT("Failed preparation preserves active delegate dispatch"),
		Session.GetLiveEventCallbackCount(),
		4);

	Session.SetLiveExecutionObserverForTesting(
		[ReplacementSource]()
		{
			ReplacementSource->Broadcast(ReplacementSource, 21, 3.5f);
		});
	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Tick completes after a reentrant delegate broadcast"), Session.Tick(0.016f, TickResult));
	TestEqual(
		TEXT("Reentrant delegate broadcast is dropped without entering the guest"),
		Session.GetLiveEventCallbackCount(),
		4);
	TestEqual(
		TEXT("Reentrant delegate broadcast does not fault the runtime"),
		Session.GetLiveLifecycleState(),
		EAvidScriptLifecycleState::Running);
	ReplacementSource->Broadcast(ReplacementSource, 22, 3.75f);
	TestEqual(
		TEXT("The subscription remains usable after the reentrant broadcast"),
		Session.GetLiveEventCallbackCount(),
		5);

	UAvidScriptRuntimeDelegateTestObject* const ExplicitSource =
		NewObject<UAvidScriptRuntimeDelegateTestObject>();
	Error.Reset();
	const int64 ExplicitToken = Session.SubscribeDelegateForTesting(
		*ExplicitSource,
		Event.EventOrdinal,
		Error);
	TestTrue(
		*FString::Printf(
			TEXT("An arbitrary compatible source receives an opaque token (error=%s)"),
			Error.IsEmpty() ? TEXT("<none>") : *Error),
		ExplicitToken > 0);
	TestEqual(
		TEXT("Explicit subscription is tracked beside the automatic self subscription"),
		Session.GetDelegateSubscriptionCountForTesting(),
		2);
	ExplicitSource->Broadcast(ExplicitSource, 23, 4.0f);
	TestEqual(
		TEXT("Arbitrary-source broadcast reaches the prepared guest export"),
		Session.GetLiveEventCallbackCount(),
		6);
	TestTrue(
		TEXT("Explicit cancellation removes the token-owned subscription"),
		Session.UnsubscribeDelegateForTesting(ExplicitToken, Error));
	TestEqual(
		TEXT("Cancellation preserves the automatic self subscription"),
		Session.GetDelegateSubscriptionCountForTesting(),
		1);
	ExplicitSource->Broadcast(ExplicitSource, 24, 4.25f);
	TestEqual(
		TEXT("Cancelled source no longer reaches the guest"),
		Session.GetLiveEventCallbackCount(),
		6);
	TestFalse(
		TEXT("A stale token is rejected without affecting the session"),
		Session.UnsubscribeDelegateForTesting(ExplicitToken, Error));
	TestEqual(
		TEXT("An incompatible arbitrary source cannot subscribe"),
		Session.SubscribeDelegateForTesting(
			*InvalidSource,
			Event.EventOrdinal,
			Error),
		static_cast<int64>(0));

	Session.UnbindDelegateSubscriptionsForTesting();
	TestEqual(
		TEXT("Explicit teardown removes the subscription"),
		Session.GetDelegateSubscriptionCountForTesting(),
		0);
	ReplacementSource->Broadcast(ReplacementSource, 25, 4.5f);
	TestEqual(
		TEXT("Broadcast after teardown does not re-enter the guest"),
		Session.GetLiveEventCallbackCount(),
		6);
	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(TEXT("Session stops cleanly"), Session.StopAndUnload(StopResult));
	return true;
}

bool FAvidScriptDelegateExportPreparationTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	FAvidScriptWasmRuntimeInstance Runtime(Selection);
	FAvidScriptWasmSmokeResult LoadResult;
	if (!TestTrue(
			TEXT("Embedded lifecycle module loads"),
			Runtime.LoadEmbeddedSmokeModule(LoadResult)))
	{
		return false;
	}

	FAvidScriptPreparedDelegateEvent MissingHandler;
	MissingHandler.StableId = FString::ChrN(64, TEXT('b'));
	MissingHandler.ExportName = TEXT("avid_on_delegate_bbbbbbbbbbbbbbbb");
	MissingHandler.Signature.ParameterCellCount = 0;
	TArray<FAvidScriptPreparedDelegateEvent> Events{MissingHandler};
	FString Error;
	TestTrue(
		TEXT("An event without a guest handler is optional"),
		Runtime.PrepareDelegateEventExportsForTesting(Events, Error));
	TestEqual(
		TEXT("Optional event is not subscribed"),
		Events.Num(),
		0);

	FAvidScriptPreparedDelegateEvent WrongSignature;
	WrongSignature.StableId = FString::ChrN(64, TEXT('c'));
	WrongSignature.ExportName = TEXT("avid_on_tick");
	WrongSignature.Signature.ParameterCellCount = 0;
	Events = {WrongSignature};
	TestFalse(
		TEXT("An implemented handler with an incompatible ABI is rejected"),
		Runtime.PrepareDelegateEventExportsForTesting(Events, Error));
	TestTrue(
		TEXT("Signature rejection identifies delegate export preparation"),
		Error.Contains(TEXT("delegate_export_prepare_failed")));

	Runtime.Unload();
	return true;
}

bool FAvidScriptSinglecastDelegateLeaseTest::RunTest(
	const FString& Parameters)
{
	UAvidScriptRuntimeDelegateTestObject* const Source =
		NewObject<UAvidScriptRuntimeDelegateTestObject>();
	FDelegateProperty* const Property = FindFProperty<FDelegateProperty>(
		Source->GetClass(),
		GET_MEMBER_NAME_CHECKED(
			UAvidScriptRuntimeDelegateTestObject,
			OnSinglecast));
	TestNotNull(TEXT("Fixture exposes a singlecast delegate property"), Property);
	if (Property == nullptr || Property->SignatureFunction == nullptr)
	{
		return false;
	}

	FAvidScriptPreparedDelegateEvent Event;
	Event.EventOrdinal = 1;
	Event.StableId = FString::ChrN(64, TEXT('d'));
	Event.ExportName = TEXT("avid_on_tick");
	Event.CallbackKind = TEXT("singlecast");
	Event.ExpectedSourceClass = Source->GetClass();
	Event.Signature.Kind = EAvidScriptPreparedDelegateKind::Singlecast;
	Event.Signature.SinglecastProperty = Property;
	Event.Signature.SignatureFunction = Property->SignatureFunction;
	Event.Signature.ParameterCellCount = 1;
	Event.Signature.ImmutableCodecIdentity = Property->SignatureFunction;
	Event.Signature.Encode = &EncodeSinglecastTestFrame;

	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult LoadResult;
	if (!TestTrue(
			TEXT("Embedded runtime reaches Running before singlecast bind"),
			Session.LoadEmbeddedSmoke(LoadResult)))
	{
		return false;
	}
	FString Error;
	if (!TestTrue(
			TEXT("Singlecast catalog prepares without mutating the property"),
			Session.PrepareDelegateSubscriptionsForTesting(
				Source,
				MakeArrayView(&Event, 1),
				Error)))
	{
		return false;
	}
	Session.CommitDelegateSubscriptionsForTesting();
	TestEqual(
		TEXT("Singlecast catalog does not auto-bind"),
		Session.GetDelegateSubscriptionCountForTesting(),
		0);

	Source->OnSinglecast.BindDynamic(
		Source,
		&UAvidScriptRuntimeDelegateTestObject::NativeSinglecastValue);
	const int64 Token = Session.SubscribeDelegateForTesting(
		*Source,
		Event.EventOrdinal,
		Error);
	TestTrue(TEXT("Explicit singlecast bind returns a token"), Token > 0);
	TestEqual(
		TEXT("Duplicate singlecast bind is rejected"),
		Session.SubscribeDelegateForTesting(
			*Source,
			Event.EventOrdinal,
			Error),
		static_cast<int64>(0));
	Source->ExecuteSinglecast(41);
	TestEqual(
		TEXT("Singlecast bridge invokes the prepared guest export"),
		Session.GetLiveEventCallbackCount(),
		1);
	TestEqual(
		TEXT("Script ownership temporarily replaces the old delegate"),
		Source->NativeSinglecastInvocationCount,
		0);
	TestTrue(
		TEXT("Cancel releases the owned singlecast lease"),
		Session.UnsubscribeDelegateForTesting(Token, Error));
	Source->ExecuteSinglecast(42);
	TestEqual(
		TEXT("Cancel restores the previous singlecast delegate"),
		Source->NativeSinglecastInvocationCount,
		1);
	TestEqual(
		TEXT("Restored delegate receives the current value"),
		Source->LastNativeSinglecastValue,
		42);

	const int64 OverwriteToken = Session.SubscribeDelegateForTesting(
		*Source,
		Event.EventOrdinal,
		Error);
	TestTrue(TEXT("A released singlecast lease can bind again"), OverwriteToken > 0);
	Source->OnSinglecast.BindDynamic(
		Source,
		&UAvidScriptRuntimeDelegateTestObject::ExternalSinglecastValue);
	TestTrue(
		TEXT("Cancel after external overwrite releases only the script bridge"),
		Session.UnsubscribeDelegateForTesting(OverwriteToken, Error));
	Source->ExecuteSinglecast(43);
	TestEqual(
		TEXT("External overwrite remains installed after script cancel"),
		Source->ExternalSinglecastInvocationCount,
		1);
	TestEqual(
		TEXT("External overwrite receives the current value"),
		Source->LastExternalSinglecastValue,
		43);

	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(TEXT("Singlecast session stops cleanly"), Session.StopAndUnload(StopResult));
	return true;
}

#endif
