#pragma once

#include "AvidScriptVmBackend.h"

struct FAvidScriptVmStaticHostImport
{
	EAvidScriptHostBindingId BindingId = EAvidScriptHostBindingId::Invalid;
	const ANSICHAR* ImportName = nullptr;
	const ANSICHAR* Signature = nullptr;
	bool bSupportsEnvCompatibility = true;
};

struct FAvidScriptVmStaticValue
{
	EAvidScriptVmValueKind Kind = EAvidScriptVmValueKind::I32;
	int32 I32 = 0;
	int64 I64 = 0;
	float F32 = 0.0f;
	double F64 = 0.0;
};

struct FAvidScriptVmStaticCallResult
{
	EAvidScriptVmValueKind Kind = EAvidScriptVmValueKind::I32;
	int32 I32 = 0;
	int64 I64 = 0;
	float F32 = 0.0f;
	double F64 = 0.0;
};

TConstArrayView<FAvidScriptVmStaticHostImport> GetAvidScriptVmStaticHostImports();
const FAvidScriptVmStaticHostImport& GetAvidScriptVmStaticHostImport(EAvidScriptHostBindingId BindingId);

bool InvokeAvidScriptVmStaticHostImport(
	const FAvidScriptVmStaticHostImport& Import,
	const FAvidScriptVmAbiSignature& Signature,
	TConstArrayView<FAvidScriptVmStaticValue> Arguments,
	IAvidScriptHostDispatcher* HostDispatcher,
	IAvidScriptVmGuestMemory& GuestMemory,
	FAvidScriptVmStaticCallResult& OutResult,
	FString& OutFailureDetails,
	FString* OutFailureCategory = nullptr);
