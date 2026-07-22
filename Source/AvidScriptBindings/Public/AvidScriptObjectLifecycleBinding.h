#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"

enum class EAvidScriptBindingInvocationKind : uint8
{
	ReflectedFunction,
	ReflectedPropertyRead,
	ObjectSpawnActor,
	ObjectDestroyActor,
	ObjectIsA
};

struct FAvidScriptObjectLifecycleBindingSpec
{
	EAvidScriptBindingInvocationKind Kind = EAvidScriptBindingInvocationKind::ReflectedFunction;
	FString StableId;
	FString ModuleName;
	FString ImportName;
	FString Signature;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptObjectLifecycleBindings
{
public:
	static TConstArrayView<FAvidScriptObjectLifecycleBindingSpec> GetSpecs();
};
