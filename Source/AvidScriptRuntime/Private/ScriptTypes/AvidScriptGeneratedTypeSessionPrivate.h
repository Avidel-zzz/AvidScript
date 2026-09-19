#pragma once

#include "AvidScriptVmBackend.h"
#include "AvidScriptWasmRuntime.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"
#include "ScriptTypes/AvidScriptGeneratedTypeAuthority.h"

class FAvidScriptGeneratedTypeRegistrySnapshot;
class FProperty;
class UClass;
class FAvidScriptRuntimeSession;


struct FAvidScriptGeneratedPreparedTypeRoute
{
	bool bEnabled = false;
	TArray<FAvidScriptContextualExportCall> Calls;
	TArray<uint8> CallShapes;
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
	FAvidScriptContextualExportCall ContinuationCall;
	TMap<FString, FAvidScriptContextualExportCall> DelegateCalls;
};
