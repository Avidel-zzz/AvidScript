#pragma once

#include "AvidScriptEditorCommandLauncher.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorToolchainSettings
{
	FString Ldc2Path;
	FString ToolchainRoot;
	FString OutputRoot;
	FString ReportRoot;
};

class FAvidScriptEditorSettingsService
{
public:
	static void ApplySettings(
		const FAvidScriptEditorToolchainSettings& Settings,
		FAvidScriptEditorCommandLaunchConfig& InOutConfig);
};
