#pragma once

#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"

class FProperty;
class UFunction;

struct FAvidScriptProjectedDelegateEvent
{
	FAvidScriptProjectedBindingValue ReturnValue;
	TArray<FAvidScriptProjectedBindingValue> Parameters;
	int32 AbiCellCount = 0;
};

class FAvidScriptEditorReflectedDelegateEventPolicy
{
public:
	static constexpr int32 MaxAbiCells = 8;

	static bool EvaluateAndProject(
		const FProperty* DelegateProperty,
		FAvidScriptProjectedDelegateEvent& OutProjection,
		FString& OutCategory,
		FString& OutSource);

	static bool EvaluateSignatureAndProject(
		const UFunction* Signature,
		FAvidScriptProjectedDelegateEvent& OutProjection,
		FString& OutCategory,
		FString& OutSource);
};
