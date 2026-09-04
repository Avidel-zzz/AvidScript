#include "Delegate/AvidScriptDelegateBridge.h"

#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace
{
constexpr int32 AvidScriptMaximumUniqueDelegateBridgeFunctions = 4096;
constexpr const TCHAR* AvidScriptDelegateBridgeFunctionPrefix = TEXT("AvidDelegate_");

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

bool HaveCompatibleDelegateParameters(
	const UFunction& Left,
	const UFunction& Right)
{
	constexpr EPropertyFlags CallingConventionFlags =
		CPF_Parm
		| CPF_OutParm
		| CPF_ReturnParm
		| CPF_ReferenceParm
		| CPF_ConstParm;
	TFieldIterator<FProperty> LeftParameter(&Left);
	TFieldIterator<FProperty> RightParameter(&Right);
	while (LeftParameter && LeftParameter->HasAnyPropertyFlags(CPF_Parm))
	{
		if (!RightParameter
			|| !RightParameter->HasAnyPropertyFlags(CPF_Parm))
		{
			return false;
		}
		const FProperty& LeftProperty = **LeftParameter;
		const FProperty& RightProperty = **RightParameter;
		if (!LeftProperty.SameType(&RightProperty)
			|| LeftProperty.GetSize() != RightProperty.GetSize()
			|| LeftProperty.ArrayDim != RightProperty.ArrayDim
			|| (LeftProperty.GetPropertyFlags() & CallingConventionFlags)
				!= (RightProperty.GetPropertyFlags() & CallingConventionFlags))
		{
			return false;
		}
		++LeftParameter;
		++RightParameter;
	}
	return !RightParameter
		|| !RightParameter->HasAnyPropertyFlags(CPF_Parm);
}

int32 CountUniqueDelegateBridgeFunctions(const UClass& BridgeClass)
{
	TArray<UObject*> ClassObjects;
	GetObjectsWithOuter(
		&BridgeClass,
		ClassObjects,
		EGetObjectsFlags::None);
	int32 Count = 0;
	for (const UObject* ClassObject : ClassObjects)
	{
		if (ClassObject != nullptr
			&& ClassObject->IsA<UFunction>()
			&& ClassObject->GetName().StartsWith(
				AvidScriptDelegateBridgeFunctionPrefix))
		{
			++Count;
		}
	}
	return Count;
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
			TEXT("%s%s"),
			AvidScriptDelegateBridgeFunctionPrefix,
			*StableId.Left(16)));
	if (UFunction* Existing = BridgeClass->FindFunctionByName(
		FunctionName,
		EIncludeSuperFlag::ExcludeSuper))
	{
		// Duplicating a delegate signature into the bridge class can relink its
		// parameter offsets. The ProcessEvent bridge consumes the original native
		// parameter block, so collision safety is defined by ABI shape, not offsets.
		if (!HaveCompatibleDelegateParameters(*Existing, SignatureFunction))
		{
			OutError = TEXT("delegate_bridge_signature_collision");
			return false;
		}
		if (!Existing->IsRooted())
		{
			Existing->AddToRoot();
		}
		OutFunction = Existing;
		return true;
	}
	if (CountUniqueDelegateBridgeFunctions(*BridgeClass)
		>= AvidScriptMaximumUniqueDelegateBridgeFunctions)
	{
		OutError = TEXT("delegate_bridge_function_limit_reached");
		return false;
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
	// Native classes can live in the Disregard-for-GC set. Any runtime function
	// referenced by their function map must therefore be in the root set first.
	Function->AddToRoot();
	BridgeClass->AddFunctionToFunctionMap(Function, FunctionName);
	OutFunction = Function;
	return true;
}
