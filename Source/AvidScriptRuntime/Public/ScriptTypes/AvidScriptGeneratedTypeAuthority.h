#pragma once

#include "CoreMinimal.h"

class FAvidScriptGeneratedTypeRegistrySnapshot;
class FAvidScriptWasmRuntimeInstance;
struct FAvidScriptObjectHandle;
struct FAvidScriptContextualExportCall;
struct FAvidScriptVmCallFrame;
struct FAvidScriptVmCallResult;
struct FAvidScriptVmError;

// Per-instance endpoint. Runtime keeps a weak reference so retained contexts
// cannot extend the lifetime of an ended or destroyed Session.
class IAvidScriptGeneratedTypeAuthority
{
public:
	virtual ~IAvidScriptGeneratedTypeAuthority() = default;
	virtual UObject* ResolveGeneratedTypeReceiver(
		int64 PackedSelf, uint32 TypeOrdinal, const FAvidScriptGeneratedTypeRegistrySnapshot& Registry) const = 0;
	// Native prepared-code capability, not an unvalidated Guest method ordinal.
	virtual bool InvokeInstanceExport(FAvidScriptWasmRuntimeInstance& SourceRuntime,
		const FAvidScriptObjectHandle& Target, const FAvidScriptContextualExportCall& Call,
		const FAvidScriptVmCallFrame& Frame, FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult) = 0;
};
