#pragma once

#include "AvidScriptWasmRuntime.h"

class FProperty;

struct FAvidScriptGeneratedReceiverHostContext
{
	FAvidScriptWasmRuntimeInstance* Runtime = nullptr;
	const FAvidScriptGeneratedTypeRegistrySnapshot* Registry = nullptr;
	uint32 TypeOrdinal = 0;
};

using FAvidScriptGeneratedPropertyI32Read = bool (*)(FProperty&, UObject&, int32&);
using FAvidScriptGeneratedPropertyI32Write = bool (*)(FProperty&, UObject&, int32);
using FAvidScriptGeneratedPropertyI64Read = bool (*)(FProperty&, UObject&, int64&);
using FAvidScriptGeneratedPropertyI64Write = bool (*)(FProperty&, UObject&, int64);
using FAvidScriptGeneratedPropertyF32Read = bool (*)(FProperty&, UObject&, float&);
using FAvidScriptGeneratedPropertyF32Write = bool (*)(FProperty&, UObject&, float);
using FAvidScriptGeneratedPropertyF64Read = bool (*)(FProperty&, UObject&, double&);
using FAvidScriptGeneratedPropertyF64Write = bool (*)(FProperty&, UObject&, double);

struct FAvidScriptGeneratedPropertyHostContext : FAvidScriptGeneratedReceiverHostContext
{
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

// Owned by the Runtime, including during Session retirement and candidate load.
struct FAvidScriptGeneratedTypeHostBindings
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	TArray<TUniquePtr<FAvidScriptGeneratedPropertyHostContext>> PropertyContexts;
	TArray<TUniquePtr<FAvidScriptGeneratedReceiverHostContext>> ReceiverContexts;
	TArray<FAvidScriptVmTypedHostImport> HostImports;
};
