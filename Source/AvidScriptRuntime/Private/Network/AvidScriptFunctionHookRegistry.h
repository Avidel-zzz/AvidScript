#pragma once

#include "CoreMinimal.h"

class UFunction;
class UObject;

class IAvidScriptFunctionHookSink
{
public:
	virtual ~IAvidScriptFunctionHookSink() = default;

	virtual void HandleAvidScriptInboundFunction(
		uint32 HandlerOrdinal,
		UFunction& Function,
		void* Parameters) = 0;
};

struct FAvidScriptFunctionHookRoute
{
	UObject* Source = nullptr;
	UFunction* Function = nullptr;
	uint32 HandlerOrdinal = MAX_uint32;
};

class FAvidScriptFunctionHookRegistry final
{
public:
	static bool ValidateReplacement(
		IAvidScriptFunctionHookSink& Sink,
		TConstArrayView<FAvidScriptFunctionHookRoute> Routes,
		FString& OutError);

	static bool ReplaceRoutes(
		IAvidScriptFunctionHookSink& Sink,
		TConstArrayView<FAvidScriptFunctionHookRoute> Routes,
		FString& OutError);

	static void RemoveRoutes(IAvidScriptFunctionHookSink& Sink);
	static int32 NumRoutes(const IAvidScriptFunctionHookSink& Sink);
};
