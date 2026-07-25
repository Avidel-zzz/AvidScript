#include "BindingGeneration/AvidScriptEditorReflectedPropertyPolicy.h"

#include "BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"
#include "UObject/Class.h"
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

bool FAvidScriptEditorReflectedPropertyPolicy::EvaluateWritable(
	const FProperty* Property,
	FString& OutDispatchMode,
	FString& OutWritePolicy,
	FString& OutCategory,
	FString& OutSource)
{
	OutDispatchMode.Empty();
	OutWritePolicy.Empty();
	if (!EvaluateReadable(Property, OutCategory, OutSource))
	{
		return false;
	}

	const EPropertyFlags RejectedFlags =
		CPF_BlueprintReadOnly
		| CPF_EditConst
		| CPF_Config
		| CPF_GlobalConfig
		| CPF_EditorOnly
		| CPF_Deprecated
		| CPF_InstancedReference
		| CPF_ContainsInstancedReference
		| CPF_Net;
	if (Property->HasAnyPropertyFlags(RejectedFlags))
	{
		OutCategory = Property->HasAnyPropertyFlags(CPF_Net)
			? TEXT("property_write_replication_unsupported")
			: TEXT("property_write_not_allowed");
		OutSource = Property->GetPathName();
		return false;
	}

	const FString BlueprintSetterName = Property->GetMetaData(TEXT("BlueprintSetter"));
	if (BlueprintSetterName.IsEmpty())
	{
		OutDispatchMode = TEXT("cached_property_set");
		OutWritePolicy = TEXT("direct");
		return true;
	}

	UClass* OwnerClass = Cast<UClass>(Property->GetOwnerStruct());
	UFunction* Setter = OwnerClass == nullptr
		? nullptr
		: OwnerClass->FindFunctionByName(FName(*BlueprintSetterName));
	FString FunctionCategory;
	FString FunctionSource;
	FAvidScriptProjectedFunction SetterProjection;
	FString ProjectionSource;
	if (Setter == nullptr
		|| Setter->GetOwnerClass() != OwnerClass
		|| Setter->HasAnyFunctionFlags(FUNC_Static)
		|| !FAvidScriptEditorReflectedFunctionPolicy::Evaluate(
			Setter,
			FunctionCategory,
			FunctionSource)
		|| !FAvidScriptEditorReflectedTypePolicy::ProjectFunction(
			Setter,
			false,
			SetterProjection,
			ProjectionSource)
		|| !SetterProjection.ReturnValue.Type.bVoid
		|| SetterProjection.Parameters.Num() != 1)
	{
		OutCategory = TEXT("property_blueprint_setter_invalid");
		OutSource = Property->GetPathName() + TEXT(":") + BlueprintSetterName;
		return false;
	}

	FAvidScriptProjectedBindingValue PropertyProjection;
	if (!FAvidScriptEditorReflectedTypePolicy::ProjectReadableProperty(
			Property,
			PropertyProjection,
			ProjectionSource)
		|| SetterProjection.Parameters[0].Type.CanonicalType
			!= PropertyProjection.Type.CanonicalType
		|| (SetterProjection.Parameters[0].Direction != TEXT("value")
			&& SetterProjection.Parameters[0].Direction != TEXT("const_ref")))
	{
		OutCategory = TEXT("property_blueprint_setter_type_mismatch");
		OutSource = Property->GetPathName() + TEXT(":") + BlueprintSetterName;
		return false;
	}

	OutDispatchMode = TEXT("cached_blueprint_setter");
	OutWritePolicy = TEXT("blueprint_setter");
	return true;
}
