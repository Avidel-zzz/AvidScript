#pragma once

#include "CoreMinimal.h"

struct FAvidScriptWasmSmokeResult
{
	bool bRuntimeInitialized = false;
	bool bModuleLoaded = false;
	bool bModuleInstantiated = false;
	bool bBeginPlayCalled = false;
	bool bTickCalled = false;
	FString ErrorMessage;
};

class FAvidScriptWasmRuntime
{
public:
	static bool RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
};

