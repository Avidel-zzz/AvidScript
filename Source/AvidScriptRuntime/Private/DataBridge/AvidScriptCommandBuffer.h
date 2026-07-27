#pragma once

#include "AvidScriptDataBridgeTypes.h"

struct FAvidScriptParsedCommand
{
	EAvidScriptCommandOpcode Opcode = EAvidScriptCommandOpcode::SetI32;
	uint32 BindingOrdinal = MAX_uint32;
	int32 SelfSlot = 0;
	int32 SelfGeneration = 0;
	int32 Arg0 = 0;
	int32 Arg1 = 0;
};

struct FAvidScriptParsedCommandBuffer
{
	uint64 CallbackEpoch = 0;
	uint32 ByteCount = 0;
	TArray<FAvidScriptParsedCommand, TInlineAllocator<32>> Commands;
};

struct FAvidScriptCommandBufferParseResult
{
	FString ErrorCategory;
	FString ErrorSource;
	FString ErrorDetails;
};

class FAvidScriptCommandBufferParser
{
public:
	static bool Parse(
		TConstArrayView<uint8> Bytes,
		uint64 ExpectedCallbackEpoch,
		const FAvidScriptDataBridgeBudget& Budget,
		FAvidScriptParsedCommandBuffer& OutBuffer,
		FAvidScriptCommandBufferParseResult& OutResult);
};
