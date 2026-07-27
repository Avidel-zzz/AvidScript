#pragma once

#include "CoreMinimal.h"

#include "AvidScriptPerfRunner.h"

class AAvidScriptPerfFixture;

struct FAvidScriptGameplayFrameCounts
{
	uint64 LogicalOperationCount = 0;
	uint64 ScalarPropertyCount = 0;
	uint64 PropertyWriteCount = 0;
	uint64 VectorCount = 0;
	uint64 ObjectCount = 0;
	uint64 EventCount = 0;
	uint64 LogicalEntityCount = 0;
};

class FAvidScriptGameplayFrameBenchmark
{
public:
	static bool IsGameplayWorkload(EAvidScriptPerfWorkload Workload);

	static FAvidScriptGameplayFrameCounts GetCounts(
		EAvidScriptPerfWorkload Workload,
		int32 Frames);

	static uint32 RunNative(
		AAvidScriptPerfFixture& Fixture,
		EAvidScriptPerfWorkload Workload,
		int32 Frames,
		uint32 Seed);
};
