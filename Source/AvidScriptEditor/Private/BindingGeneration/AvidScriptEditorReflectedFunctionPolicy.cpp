#include "BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.h"

#include "UObject/Class.h"

bool FAvidScriptEditorReflectedFunctionPolicy::Evaluate(
	const UFunction* Function,
	FString& OutCategory,
	FString& OutSource)
{
	OutCategory.Empty();
	OutSource.Empty();
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
		|| Function->HasMetaData(TEXT("Latent"))
		|| Function->HasMetaData(TEXT("CustomThunk")))
	{
		OutCategory = TEXT("function_not_allowed");
		OutSource = Function->GetPathName();
		return false;
	}
	return true;
}
