#pragma once

#include "AvidScriptBindingCodecProgram.h"
#include "AvidScriptBindingInvocation.h"

namespace UE::AvidScript::BindingPrivate
{
struct FPreparedDynamicInvocationCell
{
	const FInvocationCodecProgram* Program = nullptr;
	uint32 BindingOrdinal = MAX_uint32;
};

bool PrepareInvocationFrameLifecycle(
	FInvocationCodecProgram& Program,
	FString& OutDetails);

bool InvokePreparedDynamicReflection(
	const void* InvocationCell,
	UObject& Receiver,
	TConstArrayView<uint64> Arguments,
	IAvidScriptVmGuestMemory* GuestMemory,
	const FAvidScriptBindingInvocationContext& InvocationContext,
	TArray<uint8>& InvocationScratch,
	FAvidScriptDynamicHostCallResult& OutResult);
} // namespace UE::AvidScript::BindingPrivate
