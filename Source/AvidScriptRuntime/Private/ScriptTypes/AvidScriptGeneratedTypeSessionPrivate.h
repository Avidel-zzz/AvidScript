#pragma once

#include "AvidScriptVmBackend.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"

class FAvidScriptGeneratedTypeRegistrySnapshot;
class FProperty;
class UClass;

using FAvidScriptGeneratedPropertyI32Read = bool (*)(FProperty&, UObject&, int32&);
using FAvidScriptGeneratedPropertyI32Write = bool (*)(FProperty&, UObject&, int32);
using FAvidScriptGeneratedPropertyI64Read = bool (*)(FProperty&, UObject&, int64&);
using FAvidScriptGeneratedPropertyI64Write = bool (*)(FProperty&, UObject&, int64);
using FAvidScriptGeneratedPropertyF32Read = bool (*)(FProperty&, UObject&, float&);
using FAvidScriptGeneratedPropertyF32Write = bool (*)(FProperty&, UObject&, float);
using FAvidScriptGeneratedPropertyF64Read = bool (*)(FProperty&, UObject&, double&);
using FAvidScriptGeneratedPropertyF64Write = bool (*)(FProperty&, UObject&, double);

struct FAvidScriptGeneratedPropertyHostContext
{
	TWeakObjectPtr<UObject> Receiver;
	FAvidScriptObjectHandle ReceiverHandle;
	UClass* ExpectedClass = nullptr;
	FProperty* Property = nullptr;
	FAvidScriptGeneratedPropertyI32Read ReadI32 = nullptr;
	FAvidScriptGeneratedPropertyI32Write WriteI32 = nullptr;
	FAvidScriptGeneratedPropertyI64Read ReadI64 = nullptr;
	FAvidScriptGeneratedPropertyI64Write WriteI64 = nullptr;
	FAvidScriptGeneratedPropertyF32Read ReadF32 = nullptr;
	FAvidScriptGeneratedPropertyF32Write WriteF32 = nullptr;
	FAvidScriptGeneratedPropertyF64Read ReadF64 = nullptr;
	FAvidScriptGeneratedPropertyF64Write WriteF64 = nullptr;
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
