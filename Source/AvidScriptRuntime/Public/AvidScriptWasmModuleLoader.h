#pragma once

#include "CoreMinimal.h"

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmModuleLoadResult
{
	bool bSucceeded = false;
	FString ModulePath;
	int64 ByteSize = 0;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmModuleLoader
{
public:
	static bool LoadFromFile(
		const FString& ModulePath,
		TArray<uint8>& OutBytecode,
		FAvidScriptWasmModuleLoadResult& OutResult);
};
