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

FString FAvidScriptEditorBindingDescriptorIdentity::MakeObjectFactoryIdentity(
	const FString& ClassReferenceId,
	const EAvidScriptObjectFactoryKind Kind,
	const FString& OuterTypeId,
	const EAvidScriptObjectOwnershipPolicy Ownership,
	const EAvidScriptComponentRegistrationPolicy Registration)
{
	return FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryIdentity(
		ClassReferenceId,
		Kind,
		OuterTypeId,
		Ownership,
		Registration);
}

FString FAvidScriptEditorBindingDescriptorIdentity::MakeObjectFactoryStableId(
	const FString& ClassReferenceId,
	const EAvidScriptObjectFactoryKind Kind,
	const FString& OuterTypeId,
	const EAvidScriptObjectOwnershipPolicy Ownership,
	const EAvidScriptComponentRegistrationPolicy Registration)
{
	return FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
		ClassReferenceId,
		Kind,
		OuterTypeId,
		Ownership,
		Registration);
}
