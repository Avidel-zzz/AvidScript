#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeSession.h"
#include "Delegate/AvidScriptDelegateBridge.h"
#include "Session/AvidScriptSessionDelegateSubscriptions.h"
#include "Tests/AvidScriptDelegateSubscriptionTestTypes.h"

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
