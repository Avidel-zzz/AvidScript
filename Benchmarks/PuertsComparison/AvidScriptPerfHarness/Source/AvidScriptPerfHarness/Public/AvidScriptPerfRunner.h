#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptPerfWorkload : int32
{
	PureInteger = 0,
	ScalarNoOp = 1,
	ScalarAddInt32 = 2,
	PropertyGetSet = 3,
	VectorValue = 4,
	ObjectRoundtrip = 5,
	BatchScalar = 6
};

struct FAvidScriptPerfSmokeResult
{
	bool bSucceeded = false;
	int32 WorkloadCount = 0;
	int32 IterationsPerWorkload = 0;
	uint32 NativeChecksum = 0;
	uint32 PuertsReflectionChecksum = 0;
	uint32 PuertsStaticChecksum = 0;
	uint32 AvidScriptChecksum = 0;
	uint64 AvidScriptHostCallCount = 0;
	FString Error;
};

class AVIDSCRIPTPERFHARNESS_API FAvidScriptPerfRunner
{
public:
	static bool RunFourLaneCorrectnessSmoke(
		int32 IterationsPerWorkload,
		int32 Seed,
		FAvidScriptPerfSmokeResult& OutResult);
};
