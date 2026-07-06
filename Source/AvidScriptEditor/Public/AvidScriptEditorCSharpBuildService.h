#pragma once

#include "CoreMinimal.h"

struct FAvidScriptEditorCSharpBuildConfig
{
	FString BuildScriptPath;
	FString OutputRoot;
	FString ReportPath;
	FString DotNetPath;
	FString Configuration = TEXT("Release");
};

struct FAvidScriptEditorCSharpBuildResult
{
	bool bSucceeded = false;
	int32 ProcessExitCode = INDEX_NONE;
	FString Stdout;
	FString Stderr;
	FString ErrorCategory;
	FString ErrorMessage;
	FString BuildScriptPath;
	FString OutputRoot;
	FString ReportPath;
};

class FAvidScriptEditorCSharpBuildService
{
public:
	static FString GetDefaultActorLifecycleBuildScriptPath();
	static FString GetDefaultActorLifecycleOutputRoot();
	static FString GetDefaultActorLifecycleReportPath();

	static bool BuildActorLifecycle(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildResult& OutResult);
};
