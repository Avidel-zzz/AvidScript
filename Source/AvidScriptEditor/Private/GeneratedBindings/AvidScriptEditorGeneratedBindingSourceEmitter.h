#pragma once

#include "AvidScriptEditorGeneratedBindingService.h"

class FAvidScriptEditorGeneratedBindingSourceEmitter
{
public:
	static bool Emit(
		const FString& ProjectFile,
		const FAvidScriptGeneratedBindingPackageIr& Package,
		FAvidScriptEditorGeneratedBindingResult& OutResult);
};
