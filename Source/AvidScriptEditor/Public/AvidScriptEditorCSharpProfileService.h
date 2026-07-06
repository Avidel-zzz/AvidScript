#pragma once

#include "AvidScriptEditorCSharpBuildService.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorCSharpProfileLoadResult
{
	bool bSucceeded = false;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString NormalizedProfilePath;
	FAvidScriptEditorCSharpBuildConfig BuildConfig;
};

class FAvidScriptEditorCSharpProfileService
{
public:
	static FString GetDefaultProfilePath();

	static bool LoadProfile(
		const FString& ProfilePath,
		FAvidScriptEditorCSharpProfileLoadResult& OutResult);
};
