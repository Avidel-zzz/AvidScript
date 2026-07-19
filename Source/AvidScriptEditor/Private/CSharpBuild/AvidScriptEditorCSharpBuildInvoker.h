#pragma once

#include "AvidScriptEditorCSharpBuildService.h"

struct FAvidScriptEditorCSharpBuildInvocation
{
	FAvidScriptEditorCSharpBuildConfig Config;
	FString ExecutablePath;
	FString Parameters;
	FString WorkingDirectory;
};

class FAvidScriptEditorCSharpBuildInvoker
{
public:
	static bool Prepare(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildInvocation& OutInvocation,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool Finalize(
		const FAvidScriptEditorCSharpBuildInvocation& Invocation,
		int32 ProcessExitCode,
		const FString& Stdout,
		const FString& Stderr,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool BuildOnce(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildResult& OutResult);
};
