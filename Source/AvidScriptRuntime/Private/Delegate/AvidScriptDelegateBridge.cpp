#include "Delegate/AvidScriptDelegateBridge.h"

#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
bool IsLowerSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character)
			&& (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}
} // namespace

void UAvidScriptDelegateBridge::Initialize(
	IAvidScriptDelegateBridgeSink& InSink,
	const uint64 InSubscriptionToken,
	UFunction& InBridgeFunction)
{
	check(IsInGameThread());
	Sink = &InSink;
	SubscriptionToken = InSubscriptionToken;
	BridgeFunction = &InBridgeFunction;
}

void UAvidScriptDelegateBridge::Deactivate()
{
	check(IsInGameThread());
	Sink = nullptr;
	BridgeFunction = nullptr;
	SubscriptionToken = 0;
}

void UAvidScriptDelegateBridge::ProcessEvent(
	UFunction* Function,
	void* Parameters)
{
	if (Sink != nullptr
		&& Function != nullptr
		&& Function == BridgeFunction
		&& SubscriptionToken != 0)
	{
		Sink->HandleAvidScriptDelegateBroadcast(
			SubscriptionToken,
			Parameters);
		return;
	}
	Super::ProcessEvent(Function, Parameters);
}

bool PrepareAvidScriptDelegateBridgeFunction(
	const FString& StableId,
	const UFunction& SignatureFunction,
	UFunction*& OutFunction,
	FString& OutError)
{
	check(IsInGameThread());
	OutFunction = nullptr;
	OutError.Reset();
	if (!IsLowerSha256(StableId))
	{
		OutError = TEXT("delegate_bridge_invalid_stable_id");
		return false;
	}

	UClass* const BridgeClass = UAvidScriptDelegateBridge::StaticClass();
	const FName FunctionName(
		*FString::Printf(
			TEXT("AvidDelegate_%s"),
			*StableId.Left(16)));
	if (UFunction* Existing = BridgeClass->FindFunctionByName(
		FunctionName,
		EIncludeSuperFlag::ExcludeSuper))
	{
		if (!Existing->IsSignatureCompatibleWith(&SignatureFunction))
		{
			OutError = TEXT("delegate_bridge_signature_collision");
			return false;
		}
		OutFunction = Existing;
		return true;
	}

	UObject* const Duplicate = StaticDuplicateObject(
		&SignatureFunction,
		BridgeClass,
		FunctionName,
		RF_Public | RF_Transient,
		UFunction::StaticClass());
	UFunction* const Function = Cast<UFunction>(Duplicate);
	if (Function == nullptr)
	{
		OutError = TEXT("delegate_bridge_function_creation_failed");
		return false;
	}
	Function->FunctionFlags &= ~(FUNC_Delegate | FUNC_MulticastDelegate);
	BridgeClass->AddFunctionToFunctionMap(Function, FunctionName);
	OutFunction = Function;
	return true;
}
