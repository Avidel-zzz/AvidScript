#pragma once

#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"

class FMulticastDelegateProperty;

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
};
