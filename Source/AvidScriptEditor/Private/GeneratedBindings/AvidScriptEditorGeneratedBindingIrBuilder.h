#pragma once

#include "AvidScriptEditorGeneratedBindingService.h"

class FAvidScriptEditorGeneratedBindingIrBuilder
{
public:
	static bool Build(
		const FString& DescriptorJson,
		FAvidScriptGeneratedBindingPackageIr& OutPackage,
		FAvidScriptEditorGeneratedBindingResult& OutResult);
};
