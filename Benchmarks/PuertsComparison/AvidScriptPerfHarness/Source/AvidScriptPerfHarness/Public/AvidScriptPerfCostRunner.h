#pragma once

#include "CoreMinimal.h"

class AVIDSCRIPTPERFHARNESS_API FAvidScriptPerfCostRunner
{
public:
	static bool RunFromFiles(
		const FString& RequestPath,
		const FString& ResultPath,
		FString& OutError);
};
