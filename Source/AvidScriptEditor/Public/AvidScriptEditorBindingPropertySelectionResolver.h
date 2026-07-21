#pragma once

#include "AvidScriptEditorBindingSelectionTypes.h"
#include "CoreMinimal.h"

class AVIDSCRIPTEDITOR_API FAvidScriptEditorBindingPropertySelectionResolver
{
public:
	static bool ResolveReadable(
		const FAvidScriptBindingSelectionProfile& Profile,
		TArray<FAvidScriptReflectedPropertySelection>& OutSelections,
		FAvidScriptBindingSelectionResolveResult& OutResult);
};
