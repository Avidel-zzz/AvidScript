#pragma once

#include "AvidScriptEditorBindingSelectionTypes.h"

class AVIDSCRIPTEDITOR_API FAvidScriptEditorBindingDelegateEventSelectionResolver
{
public:
	static bool Resolve(
		const FAvidScriptBindingSelectionProfile& Profile,
		TArray<FAvidScriptReflectedDelegateEventSelection>& OutSelections,
		FAvidScriptBindingSelectionResolveResult& OutResult);
};
