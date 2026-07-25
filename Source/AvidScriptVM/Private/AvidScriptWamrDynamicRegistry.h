#pragma once

#include "AvidScriptVmBackend.h"

enum class EAvidScriptWamrRawResultKind : uint8
{
	I32,
	I64
};

struct FAvidScriptWamrRawImportAttachment
{
	FString StableId;
	FString ModuleName;
	FString ImportName;
	FString Signature;
	uint32 ParameterCount = 0;
	EAvidScriptWamrRawResultKind ResultKind = EAvidScriptWamrRawResultKind::I32;
};

struct FAvidScriptWamrDynamicRegistration
{
	const FAvidScriptWamrRawImportAttachment* Attachment = nullptr;
	uint32 Ordinal = MAX_uint32;
};

bool ValidateAvidScriptVmBindingPackage(
	const FAvidScriptVmBindingPackage& Package,
	FAvidScriptVmError& OutError);

bool AcquireAvidScriptWamrDynamicImports(
	const FAvidScriptVmBindingPackage& Package,
	TArray<FAvidScriptWamrDynamicRegistration>& OutRegistrations,
	FAvidScriptVmError& OutError);

class FAvidScriptWamrNativeRegistryScope
{
public:
	FAvidScriptWamrNativeRegistryScope();
	~FAvidScriptWamrNativeRegistryScope();

	FAvidScriptWamrNativeRegistryScope(const FAvidScriptWamrNativeRegistryScope&) = delete;
	FAvidScriptWamrNativeRegistryScope& operator=(const FAvidScriptWamrNativeRegistryScope&) = delete;
};

void ReleaseAvidScriptWamrDynamicImports(
	TArray<FAvidScriptWamrDynamicRegistration>& Registrations);

bool IsAvidScriptWamrDynamicRegistryEmpty();
