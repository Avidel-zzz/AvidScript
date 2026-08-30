#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptRuntimeSession.h"
#include "Network/AvidScriptFunctionHookRegistry.h"
#include "Tests/AvidScriptDelegateSubscriptionTestTypes.h"

#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
class FAvidScriptFunctionHookTestSink final
	: public IAvidScriptFunctionHookSink
{
public:
	virtual EAvidScriptInboundFunctionDispatch HandleAvidScriptInboundFunction(
		const uint32 HandlerOrdinal,
		UFunction& Function,
		void* Parameters) override
	{
		++InvocationCount;
		LastOrdinal = HandlerOrdinal;
		const FIntProperty* const ValueProperty =
			FindFProperty<FIntProperty>(&Function, TEXT("Value"));
		if (ValueProperty != nullptr && Parameters != nullptr)
		{
			LastValue = ValueProperty->GetPropertyValue_InContainer(Parameters);
		}
		ObservedNativeInvocationCount = ObservedSource == nullptr
			? INDEX_NONE
			: ObservedSource->NativeInvocationCount;
		return DispatchResult;
	}

	int32 InvocationCount = 0;
	uint32 LastOrdinal = MAX_uint32;
	int32 LastValue = 0;
	UAvidScriptRuntimeDelegateTestObject* ObservedSource = nullptr;
	int32 ObservedNativeInvocationCount = INDEX_NONE;
	EAvidScriptInboundFunctionDispatch DispatchResult =
		EAvidScriptInboundFunctionDispatch::Handled;
};

void InvokeNativeInboundValue(
	UAvidScriptRuntimeDelegateTestObject& Source,
	UFunction& Function,
	const int32 Value)
{
	struct FParameters
	{
		int32 Value = 0;
	};
	FParameters Parameters{Value};
	Source.ProcessEvent(&Function, &Parameters);
}

bool EncodeInboundValue(
	const void* CodecIdentity,
	const void* NativeParameters,
	const FAvidScriptBindingInvocationContext&,
	FAvidScriptVmCallFrame& OutFrame,
	TArray<FAvidScriptObjectHandle>&,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	const UFunction* const Function =
		static_cast<const UFunction*>(CodecIdentity);
	const FIntProperty* const ValueProperty = Function != nullptr
		? FindFProperty<FIntProperty>(Function, TEXT("Value"))
		: nullptr;
	if (ValueProperty == nullptr || NativeParameters == nullptr)
	{
		OutErrorCategory = TEXT("inbound_handler_test_parameter_missing");
		OutErrorDetails = TEXT("The native inbound fixture did not expose Value.");
		return false;
	}
	const int32 Value =
		ValueProperty->GetPropertyValue_InContainer(NativeParameters);
	OutFrame.CellCount = 1;
	OutFrame.Cells[0] = 0;
	FMemory::Memcpy(&OutFrame.Cells[0], &Value, sizeof(Value));
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFunctionHookRegistryLifecycleTest,
	"AvidScript.Runtime.Network.FunctionHookRegistryLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReentrantInboundHandlerQueueTest,
	"AvidScript.Runtime.Network.ReentrantInboundHandlerQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFunctionHookRegistryLifecycleTest::RunTest(
	const FString& Parameters)
{
	UAvidScriptRuntimeDelegateTestObject* const Source =
		NewObject<UAvidScriptRuntimeDelegateTestObject>();
	UAvidScriptRuntimeDelegateTestObject* const OtherSource =
		NewObject<UAvidScriptRuntimeDelegateTestObject>();
	UFunction* const Function = Source->FindFunction(
		GET_FUNCTION_NAME_CHECKED(
			UAvidScriptRuntimeDelegateTestObject,
			NativeInboundValue));
	TestNotNull(TEXT("Fixture exposes a native UFunction"), Function);
	if (Function == nullptr)
	{
		return false;
	}
	const FNativeFuncPtr Original = Function->GetNativeFunc();
	InvokeNativeInboundValue(*Source, *Function, 11);
	TestEqual(TEXT("Original thunk runs before installation"), Source->LastNativeValue, 11);

	FAvidScriptFunctionHookTestSink Sink;
	Sink.ObservedSource = Source;
	const FAvidScriptFunctionHookRoute Route{Source, Function, 7};
	FString Error;
	TestTrue(
		TEXT("Route validates without mutating the UFunction"),
		FAvidScriptFunctionHookRegistry::ValidateReplacement(
			Sink,
			MakeArrayView(&Route, 1),
			Error));
	TestTrue(TEXT("Validation preserves the original thunk"), Function->GetNativeFunc() == Original);
	TestTrue(
		TEXT("Commit installs the route"),
		FAvidScriptFunctionHookRegistry::ReplaceRoutes(
			Sink,
			MakeArrayView(&Route, 1),
			Error));
	TestEqual(TEXT("Registry tracks one sink route"), FAvidScriptFunctionHookRegistry::NumRoutes(Sink), 1);

	InvokeNativeInboundValue(*Source, *Function, 23);
	TestEqual(TEXT("Matching object dispatches the script sink"), Sink.InvocationCount, 1);
	TestEqual(TEXT("Hook preserves the handler ordinal"), Sink.LastOrdinal, static_cast<uint32>(7));
	TestEqual(TEXT("Hook exposes ProcessEvent parameter memory"), Sink.LastValue, 23);
	TestEqual(TEXT("Replace semantics skip the original thunk"), Source->NativeInvocationCount, 1);

	InvokeNativeInboundValue(*OtherSource, *Function, 31);
	TestEqual(TEXT("Unmatched objects fall back to native logic"), OtherSource->LastNativeValue, 31);
	TestEqual(TEXT("Fallback does not dispatch the script sink"), Sink.InvocationCount, 1);

	const FAvidScriptFunctionHookRoute BeforeRoute{
		Source,
		Function,
		7,
		EAvidScriptFunctionHookChainMode::Before
	};
	TestTrue(
		TEXT("Before route replaces the active mode"),
		FAvidScriptFunctionHookRegistry::ReplaceRoutes(
			Sink,
			MakeArrayView(&BeforeRoute, 1),
			Error));
	InvokeNativeInboundValue(*Source, *Function, 37);
	TestEqual(TEXT("Before dispatch observes original not yet called"), Sink.ObservedNativeInvocationCount, 1);
	TestEqual(TEXT("Before dispatch calls original after script"), Source->NativeInvocationCount, 2);
	TestEqual(TEXT("Before original receives the same parameter"), Source->LastNativeValue, 37);

	const FAvidScriptFunctionHookRoute AfterRoute{
		Source,
		Function,
		7,
		EAvidScriptFunctionHookChainMode::After
	};
	TestTrue(
		TEXT("After route replaces the active mode"),
		FAvidScriptFunctionHookRegistry::ReplaceRoutes(
			Sink,
			MakeArrayView(&AfterRoute, 1),
			Error));
	InvokeNativeInboundValue(*Source, *Function, 41);
	TestEqual(TEXT("After dispatch observes original already called"), Sink.ObservedNativeInvocationCount, 3);
	TestEqual(TEXT("After original executes exactly once"), Source->NativeInvocationCount, 3);

	Sink.DispatchResult = EAvidScriptInboundFunctionDispatch::Unavailable;
	TestTrue(
		TEXT("Unavailable fallback uses replace route"),
		FAvidScriptFunctionHookRegistry::ReplaceRoutes(
			Sink,
			MakeArrayView(&Route, 1),
			Error));
	InvokeNativeInboundValue(*Source, *Function, 43);
	TestEqual(TEXT("Unavailable route falls back to original"), Source->NativeInvocationCount, 4);

	Sink.DispatchResult = EAvidScriptInboundFunctionDispatch::Failed;
	TestTrue(
		TEXT("Failed before route is installed"),
		FAvidScriptFunctionHookRegistry::ReplaceRoutes(
			Sink,
			MakeArrayView(&BeforeRoute, 1),
			Error));
	InvokeNativeInboundValue(*Source, *Function, 47);
	TestEqual(TEXT("Failed before route suppresses original"), Source->NativeInvocationCount, 4);

	Sink.DispatchResult = EAvidScriptInboundFunctionDispatch::Deferred;
	InvokeNativeInboundValue(*Source, *Function, 53);
	TestEqual(TEXT("Deferred before route does not call original inline"), Source->NativeInvocationCount, 4);
	int32 DeferredValue = 53;
	TestTrue(
		TEXT("Deferred owner can invoke the frozen original later"),
		FAvidScriptFunctionHookRegistry::InvokeOriginal(
			Sink,
			*Source,
			*Function,
			7,
			&DeferredValue,
			Error));
	TestEqual(TEXT("Deferred original executes exactly once"), Source->NativeInvocationCount, 5);
	TestEqual(TEXT("Deferred original receives copied parameters"), Source->LastNativeValue, 53);

	FAvidScriptFunctionHookRegistry::RemoveRoutes(Sink);
	TestEqual(TEXT("Explicit teardown removes every sink route"), FAvidScriptFunctionHookRegistry::NumRoutes(Sink), 0);
	TestTrue(TEXT("Last route teardown restores the original thunk"), Function->GetNativeFunc() == Original);
	InvokeNativeInboundValue(*Source, *Function, 59);
	TestEqual(TEXT("Native logic resumes after teardown"), Source->LastNativeValue, 59);
	TestEqual(TEXT("Native invocation count resumes exactly once"), Source->NativeInvocationCount, 6);
	return true;
}

bool FAvidScriptReentrantInboundHandlerQueueTest::RunTest(
	const FString& Parameters)
{
	UAvidScriptRuntimeDelegateTestObject* const Source =
		NewObject<UAvidScriptRuntimeDelegateTestObject>();
	UFunction* const Function = Source->FindFunction(
		GET_FUNCTION_NAME_CHECKED(
			UAvidScriptRuntimeDelegateTestObject,
			NativeInboundValue));
	if (!TestNotNull(TEXT("Fixture exposes a native inbound function"), Function))
	{
		return false;
	}

	FAvidScriptPreparedDelegateEvent Handler;
	Handler.EventOrdinal = 0;
	Handler.StableId = FString::ChrN(64, TEXT('b'));
	Handler.ExportName = TEXT("avid_on_tick");
	Handler.CallbackKind = TEXT("network_rpc");
	Handler.HandlerMode = TEXT("before");
	Handler.ExpectedSourceClass = Source->GetClass();
	Handler.SignatureFunction = Function;
	Handler.Network.Mode = EAvidScriptBindingNetworkMode::Server;
	Handler.Network.bReliable = true;
	Handler.ParameterCellCount = 1;
	Handler.ImmutableCodecIdentity = Function;
	Handler.Encode = &EncodeInboundValue;

	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult LoadResult;
	if (!TestTrue(
			TEXT("Embedded runtime reaches Running before inbound installation"),
			Session.LoadEmbeddedSmoke(LoadResult)))
	{
		return false;
	}
	FString Error;
	if (!TestTrue(
			TEXT("Prepared before handler accepts the fixture"),
			Session.PrepareInboundHandlersForTesting(
				Source,
				MakeArrayView(&Handler, 1),
				Error))
		|| !TestTrue(
			TEXT("Prepared before handler commits atomically"),
			Session.CommitInboundHandlersForTesting(Error)))
	{
		return false;
	}
	TestEqual(
		TEXT("Commit activates one inbound handler"),
		Session.GetInboundHandlerCountForTesting(),
		1);

	Session.SetLiveExecutionObserverForTesting(
		[Source, Function]()
		{
			InvokeNativeInboundValue(*Source, *Function, 73);
		});
	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(
		TEXT("Tick drains a reentrant inbound call after leaving the guest guard"),
		Session.Tick(0.016f, TickResult));
	TestEqual(
		TEXT("Queued inbound call reaches the guest exactly once"),
		Session.GetLiveEventCallbackCount(),
		1);
	TestEqual(
		TEXT("Deferred before handler invokes the original exactly once"),
		Source->NativeInvocationCount,
		1);
	TestEqual(
		TEXT("Deferred original receives the deep-copied parameter"),
		Source->LastNativeValue,
		73);
	TestEqual(
		TEXT("Successful drain consumes the queue"),
		Session.GetDeferredInboundHandlerCountForTesting(),
		0);

	Session.SetLiveExecutionObserverForTesting(
		[Source, Function]()
		{
			for (int32 Index = 0; Index < 65; ++Index)
			{
				InvokeNativeInboundValue(*Source, *Function, 100 + Index);
			}
		});
	TestFalse(
		TEXT("The sixty-fifth reentrant call fails the bounded queue"),
		Session.Tick(0.016f, TickResult));
	TestEqual(
		TEXT("Queue overflow exposes a stable category"),
		TickResult.ErrorCategory,
		FString(TEXT("inbound_handler_deferred_overflow")));
	TestEqual(
		TEXT("Overflow discards queued work without partial original calls"),
		Source->NativeInvocationCount,
		1);
	TestEqual(
		TEXT("Overflow clears the bounded queue"),
		Session.GetDeferredInboundHandlerCountForTesting(),
		0);

	InvokeNativeInboundValue(*Source, *Function, 211);
	TestEqual(
		TEXT("Disabled overflow lane falls back to the original implementation"),
		Source->LastNativeValue,
		211);
	Session.UnbindInboundHandlersForTesting();
	TestEqual(
		TEXT("Explicit teardown removes the inbound route"),
		Session.GetInboundHandlerCountForTesting(),
		0);
	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(TEXT("Session stops cleanly"), Session.StopAndUnload(StopResult));
	return true;
}

#endif
