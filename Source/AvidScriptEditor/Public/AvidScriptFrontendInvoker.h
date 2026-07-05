#pragma once

#include "AvidScriptFrontendReport.h"

#include "CoreMinimal.h"

struct FAvidScriptFrontendInvocationConfig
{
	FString WrapperPath;
	FString SourcePath;
	FString BindingsPath;
	FString OutputRoot;
	FString ReportPath;
	FString Ldc2Path;
	FString ToolchainRoot;
	bool bSkipCompile = false;
};

struct FAvidScriptFrontendInvocationResult
{
	bool bSucceeded = false;
	int32 ProcessExitCode = INDEX_NONE;
	FString Stdout;
	FString Stderr;
	FString ErrorCategory;
	FString ErrorMessage;
	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult ReportLoadResult;
};

class FAvidScriptFrontendInvoker
{
public:
	static bool Invoke(
		const FAvidScriptFrontendInvocationConfig& Config,
		FAvidScriptFrontendInvocationResult& OutResult);

	static FString GetDefaultWrapperPath();
};
