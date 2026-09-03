#pragma once

#include "CoreMinimal.h"
#include "AvidScriptObjectRegistry.h"

class UClass;
class UObject;
struct FAvidScriptBindingInvocationContext;

AVIDSCRIPTBINDINGS_API bool ResolveAvidScriptBindingReceiver(
	const FAvidScriptObjectHandle& Handle,
	UClass* ExpectedClass,
	const FAvidScriptBindingInvocationContext& Context,
	UObject*& OutObject,
	FString& OutDetails);
