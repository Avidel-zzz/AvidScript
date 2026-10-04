#pragma once

#include "AvidScriptVmBackend.h"
#include "AvidScriptWasmRuntime.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"
#include "ScriptTypes/AvidScriptGeneratedTypeAuthority.h"

class FAvidScriptGeneratedTypeRegistrySnapshot;
class FProperty;
class UClass;
class FAvidScriptRuntimeSession;


enum class EAvidScriptGeneratedNativeScalar : uint8
{
	Invalid,
	Void,
	I32,
	F32,
	Bool,
	I64,
	F64,
};

struct FAvidScriptGeneratedPreparedMemberRoute
{
	FAvidScriptContextualExportCall Call;
	TArray<EAvidScriptGeneratedNativeScalar> Parameters;
	EAvidScriptGeneratedNativeScalar Result = EAvidScriptGeneratedNativeScalar::Invalid;
};

struct FAvidScriptGeneratedPreparedTypeRoute
{
	bool bEnabled = false;
	TArray<FAvidScriptGeneratedPreparedMemberRoute> Members;
};

struct FAvidScriptRuntimeGeneratedTypeInstanceState
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	TWeakObjectPtr<UObject> Receiver;
	FAvidScriptObjectHandle ReceiverHandle;
	uint32 TypeOrdinal = 0;
	FAvidScriptGeneratedTypeInstanceRegistration Registration;
	TSharedPtr<IAvidScriptGeneratedTypeAuthority> Authority;
	TArray<FAvidScriptGeneratedPreparedTypeRoute> PreparedTypeRoutes;
	// Native activation actually entered for this UObject, in dispatch order.
	// Loading a VM alone must not synthesize Actor BeginPlay or subsystem Initialize.
	TArray<TPair<uint32, uint32>> ActivatedLifecycleRoutes;
	FAvidScriptContextualExportCall ContinuationCall;
	TMap<FString, FAvidScriptContextualExportCall> DelegateCalls;
};
