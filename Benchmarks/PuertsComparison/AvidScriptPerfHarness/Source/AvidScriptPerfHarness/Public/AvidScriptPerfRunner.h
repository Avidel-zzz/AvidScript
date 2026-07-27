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
	BatchScalar = 6,
	CallbackEmpty = 7,
	CallbackTick = 8,
	VectorRefOut = 9,
	GameplayFrameSmall = 10,
	GameplayFrameDense = 11,
	Count = 12
};

struct FAvidScriptPerfSmokeResult
{
	bool bSucceeded = false;
	int32 WorkloadCount = 0;
	int32 IterationsPerWorkload = 0;
	uint32 NativeChecksum = 0;
	uint32 PuertsReflectionChecksum = 0;
	uint32 PuertsStaticChecksum = 0;
	uint32 AvidScriptWasmtimeSemanticChecksum = 0;
	uint32 AvidScriptWasmtimeNativeDirectChecksum = 0;
	uint64 AvidScriptWasmtimeSemanticHostCallCount = 0;
	uint64 AvidScriptWasmtimeNativeDirectHostCallCount = 0;
	FString Error;
};

class AVIDSCRIPTPERFHARNESS_API FAvidScriptPerfRunner
{
public:
	static bool RunWarmBenchmarkFromFiles(
		const FString& RequestPath,
		const FString& ResultPath,
		FString& OutError);

	static bool RunFiveLaneCorrectnessSmoke(
		int32 IterationsPerWorkload,
		int32 Seed,
		FAvidScriptPerfSmokeResult& OutResult);
};
