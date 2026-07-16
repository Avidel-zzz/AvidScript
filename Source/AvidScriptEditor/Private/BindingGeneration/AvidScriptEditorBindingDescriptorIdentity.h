#pragma once

#include "CoreMinimal.h"

struct FAvidScriptBindingEnumValue
{
	FString Name;
	int64 Value = 0;
};

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
