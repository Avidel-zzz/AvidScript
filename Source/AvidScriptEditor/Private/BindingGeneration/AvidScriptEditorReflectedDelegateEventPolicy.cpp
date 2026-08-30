#include "BindingGeneration/AvidScriptEditorReflectedDelegateEventPolicy.h"

#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

namespace
{
bool CountProjectedTypeCells(
	const FAvidScriptProjectedBindingType& Type,
	int32& OutCellCount)
{
	OutCellCount = 0;
	if (Type.Kind == TEXT("name_utf8")
		|| Type.Kind == TEXT("string_utf8")
		|| Type.Kind == TEXT("array")
		|| Type.Kind == TEXT("void"))
	{
		return false;
	}
	if (Type.Kind == TEXT("struct_wire"))
	{
		for (const TSharedPtr<FAvidScriptProjectedBindingType>& FieldType :
			Type.StructFieldTypes)
		{
			int32 FieldCellCount = 0;
			if (!FieldType.IsValid()
				|| !CountProjectedTypeCells(*FieldType, FieldCellCount))
			{
				return false;
			}
			OutCellCount += FieldCellCount;
		}
		return OutCellCount > 0;
	}
	for (const FString& AbiType : Type.AbiValueTypes)
	{
		if (AbiType == TEXT("i") || AbiType == TEXT("f"))
		{
			++OutCellCount;
		}
		else if (AbiType == TEXT("I") || AbiType == TEXT("F"))
		{
			OutCellCount += 2;
		}
		else
		{
			return false;
		}
	}
	return OutCellCount > 0;
}
} // namespace

bool FAvidScriptEditorReflectedDelegateEventPolicy::EvaluateAndProject(
	const FMulticastDelegateProperty* DelegateProperty,
	FAvidScriptProjectedDelegateEvent& OutProjection,
	FString& OutCategory,
	FString& OutSource)
{
	OutProjection = FAvidScriptProjectedDelegateEvent();
	OutCategory.Reset();
	OutSource.Reset();
	if (DelegateProperty == nullptr
		|| DelegateProperty->SignatureFunction == nullptr)
	{
		OutCategory = TEXT("delegate_event_signature_missing");
		OutSource = DelegateProperty == nullptr
			? FString(TEXT("<null>"))
			: DelegateProperty->GetPathName();
		return false;
	}

	const bool bProjected = EvaluateSignatureAndProject(
		DelegateProperty->SignatureFunction,
		OutProjection,
		OutCategory,
		OutSource);
	if (!bProjected && OutCategory.StartsWith(TEXT("callback_")))
	{
		OutCategory = TEXT("delegate_event_")
			+ OutCategory.RightChop(9);
	}
	return bProjected;
}

bool FAvidScriptEditorReflectedDelegateEventPolicy::
	EvaluateSignatureAndProject(
		const UFunction* Signature,
		FAvidScriptProjectedDelegateEvent& OutProjection,
		FString& OutCategory,
		FString& OutSource)
{
	OutProjection = FAvidScriptProjectedDelegateEvent();
	OutCategory.Reset();
	OutSource.Reset();
	if (Signature == nullptr)
	{
		OutCategory = TEXT("callback_signature_missing");
		OutSource = TEXT("<null>");
		return false;
	}
	if (Signature->GetReturnProperty() != nullptr)
	{
		OutCategory = TEXT("callback_return_unsupported");
		OutSource = Signature->GetPathName();
		return false;
	}
	FAvidScriptProjectedFunction FunctionProjection;
	FString ProjectionError;
	if (!FAvidScriptEditorReflectedTypePolicy::ProjectFunction(
			Signature,
			true,
			FunctionProjection,
			ProjectionError))
	{
		OutCategory = TEXT("callback_type_unsupported");
		OutSource = Signature->GetPathName() + TEXT(":") + ProjectionError;
		return false;
	}

	bool bHasOutputs = false;
	for (FAvidScriptProjectedBindingValue& Parameter :
		FunctionProjection.Parameters)
	{
		if (Parameter.bHasDefaultValue)
		{
			OutCategory = TEXT("callback_default_unsupported");
			OutSource = Signature->GetPathName() + TEXT(".")
				+ Parameter.Name;
			return false;
		}
		if (Parameter.Direction != TEXT("value")
			&& Parameter.Direction != TEXT("const_ref")
			&& Parameter.Direction != TEXT("ref")
			&& Parameter.Direction != TEXT("out"))
		{
			OutCategory = TEXT("callback_direction_unsupported");
			OutSource = Signature->GetPathName() + TEXT(".")
				+ Parameter.Name;
			return false;
		}
		int32 ParameterCellCount = 0;
		if (!CountProjectedTypeCells(Parameter.Type, ParameterCellCount))
		{
			OutCategory = TEXT("callback_type_unsupported");
			OutSource = Signature->GetPathName() + TEXT(".")
				+ Parameter.Name + TEXT(":") + Parameter.Type.CanonicalType;
			return false;
		}
		const bool bOutput = Parameter.Direction == TEXT("ref")
			|| Parameter.Direction == TEXT("out");
		if (bOutput && !bHasOutputs)
		{
			++OutProjection.AbiCellCount;
			bHasOutputs = true;
		}
		if (Parameter.Direction != TEXT("out"))
		{
			OutProjection.AbiCellCount += ParameterCellCount;
		}
		if (OutProjection.AbiCellCount > MaxAbiCells)
		{
			OutCategory = TEXT("callback_abi_cells_exceeded");
			OutSource = FString::Printf(
				TEXT("%s:%d>%d"),
				*Signature->GetPathName(),
				OutProjection.AbiCellCount,
				MaxAbiCells);
			return false;
		}
		OutProjection.Parameters.Add(MoveTemp(Parameter));
	}
	return true;
}
