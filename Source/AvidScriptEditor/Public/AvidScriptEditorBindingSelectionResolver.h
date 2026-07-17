#pragma once

#include "AvidScriptEditorBindingSelectionTypes.h"

class AVIDSCRIPTEDITOR_API FAvidScriptEditorBindingSelectionResolver
{
public:
	static bool Resolve(
		const FAvidScriptBindingSelectionProfile& Profile,
		TArray<FAvidScriptReflectedFunctionSelection>& OutSelections,
		FAvidScriptBindingSelectionResolveResult& OutResult);
};
