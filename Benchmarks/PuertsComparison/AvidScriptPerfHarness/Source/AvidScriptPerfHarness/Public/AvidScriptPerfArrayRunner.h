#pragma once

#include "CoreMinimal.h"

class AVIDSCRIPTPERFHARNESS_API FAvidScriptPerfArrayRunner
{
public:
	static bool RunFromFiles(
		const FString& RequestPath,
		const FString& ResultPath,
		FString& OutError);
};
