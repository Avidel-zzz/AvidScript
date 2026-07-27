#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptCommandOpcode : uint16
{
	SetI32 = 1,
	SetVector = 2,
	InvokeI32Pair = 3,
	AssignObject = 4
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptDataBridgeBudget
{
	uint32 MaxCommands = 4096;
	uint32 MaxBytes = 256 * 1024;
	uint32 MaxObjects = 2048;
	double MaxApplyMilliseconds = 2.0;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptDataBridgeMetrics
{
	uint64 BoundaryCrossings = 0;
	uint64 SubmittedBuffers = 0;
	uint64 SubmittedCommands = 0;
	uint64 AppliedCommands = 0;
	uint64 RejectedBuffers = 0;
	uint64 SubmittedBytes = 0;
};

namespace AvidScriptDataBridgeAbi
{
	inline constexpr uint32 CommandBufferMagic = 0x41564342;
	inline constexpr uint16 CommandBufferSchemaVersion = 1;
	inline constexpr uint32 HeaderBytes = 24;
	inline constexpr uint32 CommandRecordBytes = 32;
}
