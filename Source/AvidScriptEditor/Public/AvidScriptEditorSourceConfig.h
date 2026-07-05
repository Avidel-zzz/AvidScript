#pragma once

#include "AvidScriptEditorCommandLauncher.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorSourceConfigRequest
{
	FString SourcePath;
	FString BindingsPath;
	FString OutputRoot;
	FString ReportPath;
};

struct FAvidScriptEditorSourceConfigResult
{
	bool bSucceeded = false;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString NormalizedSourcePath;
	FString SourceId;
	FAvidScriptEditorCommandLaunchConfig LaunchConfig;
};

class FAvidScriptEditorSourceConfigService
{
public:
	static bool BuildLaunchConfig(
		const FAvidScriptEditorSourceConfigRequest& Request,
		FAvidScriptEditorSourceConfigResult& OutResult);
};
