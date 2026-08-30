#pragma once

#include "AvidScriptVmBackend.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"

class FAvidScriptGeneratedTypeRegistrySnapshot;

struct FAvidScriptRuntimeGeneratedTypeInstanceState
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	TWeakObjectPtr<UObject> Receiver;
	FAvidScriptObjectHandle ReceiverHandle;
	uint32 TypeOrdinal = 0;
	FAvidScriptGeneratedTypeInstanceRegistration Registration;
	TArray<FAvidScriptVmPreparedExportCall> PreparedCalls;
	TArray<uint8> CallShapes;
};
