#include "BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.h"

#include "Engine/LatentActionManager.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

bool FAvidScriptEditorReflectedFunctionPolicy::Evaluate(
	const UFunction* Function,
	FString& OutCategory,
	FString& OutSource,
	FAvidScriptEditorLatentFunctionContract* OutLatentContract)
{
	OutCategory.Empty();
	OutSource.Empty();
	if (OutLatentContract != nullptr)
	{
		*OutLatentContract = {};
	}
	if (Function == nullptr)
	{
		OutCategory = TEXT("function_missing");
		return false;
	}
	if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
	{
		OutCategory = TEXT("function_not_callable");
		OutSource = Function->GetPathName();
		return false;
	}
	if (Function->HasAnyFunctionFlags(
		FUNC_EditorOnly | FUNC_Delegate | FUNC_MulticastDelegate | FUNC_NetRequest | FUNC_NetResponse)
		|| Function->HasMetaData(TEXT("CustomThunk")))
	{
		OutCategory = TEXT("function_not_allowed");
		OutSource = Function->GetPathName();
		return false;
	}
	if (!Function->HasMetaData(TEXT("Latent")))
	{
		return true;
	}

	const FString LatentInfoParameter = Function->GetMetaData(TEXT("LatentInfo"));
	const FString WorldContextParameter = Function->GetMetaData(TEXT("WorldContext"));
	const FStructProperty* LatentInfoProperty = LatentInfoParameter.IsEmpty()
		? nullptr
		: FindFProperty<FStructProperty>(Function, FName(*LatentInfoParameter));
	const FObjectPropertyBase* WorldContextProperty = WorldContextParameter.IsEmpty()
		? nullptr
		: FindFProperty<FObjectPropertyBase>(Function, FName(*WorldContextParameter));
	if (Function->GetReturnProperty() != nullptr
		|| LatentInfoProperty == nullptr
		|| LatentInfoProperty->Struct != FLatentActionInfo::StaticStruct()
		|| !LatentInfoProperty->HasAnyPropertyFlags(CPF_Parm)
		|| LatentInfoProperty->HasAnyPropertyFlags(CPF_ReturnParm)
		|| (!WorldContextParameter.IsEmpty()
			&& (WorldContextProperty == nullptr
				|| !WorldContextProperty->HasAnyPropertyFlags(CPF_Parm)
				|| WorldContextProperty->HasAnyPropertyFlags(CPF_ReturnParm))))
	{
		OutCategory = TEXT("latent_contract_invalid");
		OutSource = Function->GetPathName();
		return false;
	}

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		const FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Parm)
			|| Property->HasAnyPropertyFlags(CPF_ReturnParm)
			|| Property == LatentInfoProperty
			|| Property == WorldContextProperty)
		{
			continue;
		}
		if (Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)
			|| CastField<FDelegateProperty>(Property) != nullptr
			|| CastField<FMulticastDelegateProperty>(Property) != nullptr)
		{
			OutCategory = TEXT("latent_completion_shape_unsupported");
			OutSource = Function->GetPathName() + TEXT(":") + Property->GetName();
			return false;
		}
	}

	if (OutLatentContract != nullptr)
	{
		OutLatentContract->bLatent = true;
		OutLatentContract->LatentInfoParameter = LatentInfoParameter;
		OutLatentContract->WorldContextParameter = WorldContextParameter;
	}
	return true;
}
