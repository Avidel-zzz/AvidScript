#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeSession.h"
#include "Delegate/AvidScriptDelegateBridge.h"
#include "Tests/AvidScriptDelegateSubscriptionTestTypes.h"

#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

namespace
{
bool EncodeDelegateTestFrame(
	const void* CodecIdentity,
	const void* NativeParameters,
	const FAvidScriptBindingInvocationContext&,
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptDelegateBridgeLifecycleTest,
	"AvidScript.Runtime.DelegateSubscription.BridgeLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptDelegateExportPreparationTest,
	"AvidScript.Runtime.DelegateSubscription.ExportPreparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptDelegateBridgeLifecycleTest::RunTest(const FString& Parameters)
{
	UAvidScriptRuntimeDelegateTestObject* const Source =
		NewObject<UAvidScriptRuntimeDelegateTestObject>();
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
	Event.DelegateProperty = Property;
	Event.SignatureFunction = Property->SignatureFunction;
	Event.ParameterCellCount = 1;
	Event.ImmutableCodecIdentity = Property->SignatureFunction;
	Event.Encode = &EncodeDelegateTestFrame;

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
			Source,
			MakeArrayView(&Event, 1),
			Error));
	Session.CommitDelegateSubscriptionsForTesting();
	TestEqual(
		TEXT("Commit activates one subscription"),
		Session.GetDelegateSubscriptionCountForTesting(),
		1);

	Source->Broadcast(Source, 17, 2.5f);
	TestEqual(
		TEXT("Real UE multicast broadcast invokes one prepared guest export"),
		Session.GetLiveEventCallbackCount(),
		1);
	Session.UnbindDelegateSubscriptionsForTesting();
	TestEqual(
		TEXT("Explicit teardown removes the subscription"),
		Session.GetDelegateSubscriptionCountForTesting(),
		0);
	Source->Broadcast(Source, 19, 3.5f);
	TestEqual(
		TEXT("Broadcast after teardown does not re-enter the guest"),
		Session.GetLiveEventCallbackCount(),
		1);
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
	MissingHandler.ParameterCellCount = 0;
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
	WrongSignature.ParameterCellCount = 0;
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

#endif
