#pragma once

#include "AvidScriptVmBackend.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"

class FAvidScriptGeneratedTypeRegistrySnapshot;
class FFloatProperty;
class UClass;

struct FAvidScriptGeneratedPropertyHostContext
{
	TWeakObjectPtr<UObject> Receiver;
	FAvidScriptObjectHandle ReceiverHandle;
	UClass* ExpectedClass = nullptr;
	FFloatProperty* Property = nullptr;
};

struct FAvidScriptRuntimeGeneratedTypeInstanceState
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	TWeakObjectPtr<UObject> Receiver;
	FAvidScriptObjectHandle ReceiverHandle;
	uint32 TypeOrdinal = 0;
	FAvidScriptGeneratedTypeInstanceRegistration Registration;
	TArray<TUniquePtr<FAvidScriptGeneratedPropertyHostContext>> PropertyContexts;
	TArray<FAvidScriptVmTypedHostImport> PropertyImports;
	TArray<FAvidScriptVmPreparedExportCall> PreparedCalls;
	TArray<uint8> CallShapes;
};
