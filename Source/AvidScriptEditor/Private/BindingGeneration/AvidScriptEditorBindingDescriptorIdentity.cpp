#include "BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.h"

#include "AvidScriptHash.h"

FString FAvidScriptEditorBindingDescriptorIdentity::MakeTypeIdentity(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues)
{
	FString Identity = CanonicalType;
	for (const FAvidScriptBindingEnumValue& EnumValue : EnumValues)
	{
		Identity += FString::Printf(
			TEXT("|enum:%d:%s:%lld"),
			EnumValue.Name.Len(),
			*EnumValue.Name,
			EnumValue.Value);
	}
	return Identity;
}

FString FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues)
{
	return FAvidScriptHash::Sha256HexUtf8(MakeTypeIdentity(CanonicalType, EnumValues));
}
