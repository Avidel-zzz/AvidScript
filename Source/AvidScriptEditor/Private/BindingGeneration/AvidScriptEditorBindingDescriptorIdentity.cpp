#include "BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.h"

FString FAvidScriptEditorBindingDescriptorIdentity::MakeTypeIdentity(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues)
{
	return FAvidScriptBindingDescriptorIdentity::MakeTypeIdentity(CanonicalType, EnumValues);
}

FString FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues)
{
	return FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(CanonicalType, EnumValues);
}
