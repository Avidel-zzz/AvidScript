#pragma once

#include "AvidScriptBindingDescriptor.h"

class FAvidScriptEditorBindingDescriptorIdentity
{
public:
	static FString MakeTypeIdentity(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues,
		const TArray<FAvidScriptBindingStructFieldModel>& StructFields = {});

	static FString MakeTypeStableId(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues,
		const TArray<FAvidScriptBindingStructFieldModel>& StructFields = {});

	static FString MakeObjectFactoryIdentity(
		const FString& ClassReferenceId,
		EAvidScriptObjectFactoryKind Kind,
		const FString& OuterTypeId,
		EAvidScriptObjectOwnershipPolicy Ownership,
		EAvidScriptComponentRegistrationPolicy Registration);

	static FString MakeObjectFactoryStableId(
		const FString& ClassReferenceId,
		EAvidScriptObjectFactoryKind Kind,
		const FString& OuterTypeId,
		EAvidScriptObjectOwnershipPolicy Ownership,
		EAvidScriptComponentRegistrationPolicy Registration);
};
