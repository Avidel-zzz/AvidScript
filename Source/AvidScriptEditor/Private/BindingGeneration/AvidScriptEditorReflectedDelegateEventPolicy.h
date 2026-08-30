#pragma once

#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"

class FMulticastDelegateProperty;
class UFunction;

struct FAvidScriptProjectedDelegateEvent
{
	TArray<FAvidScriptProjectedBindingValue> Parameters;
	int32 AbiCellCount = 0;
};

class FAvidScriptEditorReflectedDelegateEventPolicy
{
public:
	static constexpr int32 MaxAbiCells = 8;

	static bool EvaluateAndProject(
		const FMulticastDelegateProperty* DelegateProperty,
		FAvidScriptProjectedDelegateEvent& OutProjection,
		FString& OutCategory,
		FString& OutSource);

	static bool EvaluateSignatureAndProject(
		const UFunction* Signature,
		FAvidScriptProjectedDelegateEvent& OutProjection,
		FString& OutCategory,
		FString& OutSource);
};
