#include "BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.h"

FString FAvidScriptEditorBindingDescriptorIdentity::MakeTypeIdentity(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues,
	const TArray<FAvidScriptBindingStructFieldModel>& StructFields)
{
	return FAvidScriptBindingDescriptorIdentity::MakeTypeIdentity(
		CanonicalType,
		EnumValues,
		StructFields);
}

FString FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues,
	const TArray<FAvidScriptBindingStructFieldModel>& StructFields)
{
	return FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		CanonicalType,
		EnumValues,
		StructFields);
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
