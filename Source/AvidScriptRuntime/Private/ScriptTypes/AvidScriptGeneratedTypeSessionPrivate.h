#pragma once

#include "AvidScriptVmBackend.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"
#include "ScriptTypes/AvidScriptGeneratedTypeAuthority.h"

class FAvidScriptGeneratedTypeRegistrySnapshot;
class FProperty;
class UClass;
class FAvidScriptRuntimeSession;


struct FAvidScriptGeneratedPreparedTypeRoute
{
	bool bEnabled = false;
	TArray<FAvidScriptVmPreparedExportCall> Calls;
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
};
