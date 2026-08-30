#if WITH_DEV_AUTOMATION_TESTS

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
	virtual void HandleAvidScriptInboundFunction(
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
	}

	int32 InvocationCount = 0;
	uint32 LastOrdinal = MAX_uint32;
	int32 LastValue = 0;
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFunctionHookRegistryLifecycleTest,
	"AvidScript.Runtime.Network.FunctionHookRegistryLifecycle",
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

	FAvidScriptFunctionHookRegistry::RemoveRoutes(Sink);
	TestEqual(TEXT("Explicit teardown removes every sink route"), FAvidScriptFunctionHookRegistry::NumRoutes(Sink), 0);
	TestTrue(TEXT("Last route teardown restores the original thunk"), Function->GetNativeFunc() == Original);
	InvokeNativeInboundValue(*Source, *Function, 47);
	TestEqual(TEXT("Native logic resumes after teardown"), Source->LastNativeValue, 47);
	TestEqual(TEXT("Native invocation count resumes exactly once"), Source->NativeInvocationCount, 2);
	return true;
}

#endif
