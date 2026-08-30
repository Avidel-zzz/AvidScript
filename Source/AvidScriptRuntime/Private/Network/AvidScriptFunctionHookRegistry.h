#pragma once

#include "CoreMinimal.h"

class UFunction;
class UObject;

enum class EAvidScriptFunctionHookChainMode : uint8
{
	Replace,
	Before,
	After
};

enum class EAvidScriptInboundFunctionDispatch : uint8
{
	Handled,
	Deferred,
	Failed,
	Unavailable
};

class IAvidScriptFunctionHookSink
{
public:
	virtual ~IAvidScriptFunctionHookSink() = default;

	virtual EAvidScriptInboundFunctionDispatch HandleAvidScriptInboundFunction(
		uint32 HandlerOrdinal,
		UFunction& Function,
		void* Parameters) = 0;
};

struct FAvidScriptFunctionHookRoute
{
	UObject* Source = nullptr;
	UFunction* Function = nullptr;
	uint32 HandlerOrdinal = MAX_uint32;
	EAvidScriptFunctionHookChainMode ChainMode =
		EAvidScriptFunctionHookChainMode::Replace;
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
	static bool InvokeOriginal(
		IAvidScriptFunctionHookSink& Sink,
		UObject& Source,
		UFunction& Function,
		uint32 HandlerOrdinal,
		void* Parameters,
		FString& OutError);
};
