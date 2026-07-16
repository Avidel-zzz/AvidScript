#pragma once

#include "AvidScriptBindingDescriptor.h"

class FAvidScriptEditorBindingDescriptorIdentity
{
public:
	static FString MakeTypeIdentity(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues);

	static FString MakeTypeStableId(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues);
};
