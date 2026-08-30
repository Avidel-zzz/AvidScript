#pragma once

#include "AvidScriptBindingDescriptor.h"

class FAvidScriptEditorBindingDescriptorIdentity
{
public:
	static FString MakeTypeIdentity(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues,
		const TArray<FAvidScriptBindingStructFieldModel>& StructFields = {},
		int32 WireSize = INDEX_NONE,
		int32 WireAlignment = INDEX_NONE,
		const FString& ElementTypeId = FString(),
		const TArray<FString>& TypeArguments = {});

	static FString MakeTypeStableId(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues,
		const TArray<FAvidScriptBindingStructFieldModel>& StructFields = {},
		int32 WireSize = INDEX_NONE,
		int32 WireAlignment = INDEX_NONE,
		const FString& ElementTypeId = FString(),
		const TArray<FString>& TypeArguments = {});

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
