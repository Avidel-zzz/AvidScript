#include "BindingGeneration/AvidScriptEditorReflectedPropertyPolicy.h"

#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"
#include "UObject/UnrealType.h"

bool FAvidScriptEditorReflectedPropertyPolicy::EvaluateReadable(
	const FProperty* Property,
	FString& OutCategory,
	FString& OutSource)
{
	OutCategory.Empty();
	OutSource.Empty();
	if (Property == nullptr)
	{
		OutCategory = TEXT("property_missing");
		OutSource = TEXT("<null>");
		return false;
	}
	if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible)
		|| Property->HasAnyPropertyFlags(CPF_Parm | CPF_EditorOnly | CPF_Deprecated))
	{
		OutCategory = TEXT("property_not_runtime_visible");
		OutSource = Property->GetPathName();
		return false;
	}

	FAvidScriptProjectedBindingValue Projection;
	if (!FAvidScriptEditorReflectedTypePolicy::ProjectReadableProperty(
		Property,
		Projection,
		OutSource))
	{
		OutCategory = TEXT("unsupported_property_type");
		if (OutSource.IsEmpty())
		{
			OutSource = Property->GetCPPType();
		}
		return false;
	}
	return true;
}
