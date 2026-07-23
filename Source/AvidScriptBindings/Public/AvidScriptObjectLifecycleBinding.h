#pragma once

#include "AvidScriptBindingInvocationKind.h"
#include "Containers/ArrayView.h"
#include "CoreMinimal.h"

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
