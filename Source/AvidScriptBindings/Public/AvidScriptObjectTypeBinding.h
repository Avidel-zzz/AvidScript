#pragma once

#include "AvidScriptBindingInvocationKind.h"
#include "Containers/ArrayView.h"
#include "CoreMinimal.h"

struct FAvidScriptObjectTypeBindingSpec
{
	EAvidScriptBindingInvocationKind Kind = EAvidScriptBindingInvocationKind::ObjectTypeIsA;
	FString StableId;
	FString ModuleName;
	FString ImportName;
	FString Signature;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptObjectTypeBindings
{
public:
	static TConstArrayView<FAvidScriptObjectTypeBindingSpec> GetSpecs();
};
