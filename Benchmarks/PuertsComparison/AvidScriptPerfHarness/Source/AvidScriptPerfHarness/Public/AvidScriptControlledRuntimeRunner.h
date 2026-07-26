#pragma once

#include "CoreMinimal.h"

struct FAvidScriptControlledRuntimeSmokeResult
{
	bool bSucceeded = false;
	bool bPuertsExecutedWebAssembly = false;
	FString KernelWasmSha256;
	int32 Expected = 0;
	int32 NativeResult = 0;
	int32 PuertsV8Result = 0;
	int32 WamrResult = 0;
	int32 WasmtimeResult = 0;
	FString Error;
};

class AVIDSCRIPTPERFHARNESS_API FAvidScriptControlledRuntimeRunner
{
public:
	static bool RunFromFiles(
		const FString& RequestPath,
		const FString& ResultPath,
		FString& OutError);

	static bool RunCorrectnessSmoke(
		int32 Iterations,
		int32 Seed,
		FAvidScriptControlledRuntimeSmokeResult& OutResult);
};
