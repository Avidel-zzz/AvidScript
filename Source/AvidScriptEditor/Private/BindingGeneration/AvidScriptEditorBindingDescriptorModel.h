#pragma once

#include "AvidScriptBindingDescriptor.h"

class FAvidScriptEditorBindingDescriptorModelParser
{
public:
	static bool Parse(
		const FString& Json,
		FAvidScriptBindingPackageModel& OutPackage,
		FString& OutErrorCategory,
		FString& OutErrorSource);
};
