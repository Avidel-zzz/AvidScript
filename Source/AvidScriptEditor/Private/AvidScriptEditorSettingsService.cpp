#include "AvidScriptEditorSettingsService.h"

#include "Misc/Paths.h"

namespace
{
FString NormalizeAvidScriptEditorSettingsPath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}

	return Path;
}

FString GetAvidScriptEditorSettingsSourceId(const FAvidScriptEditorCommandLaunchConfig& Config)
{
	const FString BaseName = FPaths::GetBaseFilename(Config.SourcePath);
	return BaseName.IsEmpty() ? FString(TEXT("frontend")) : BaseName;
}
} // namespace

void FAvidScriptEditorSettingsService::ApplySettings(
	const FAvidScriptEditorToolchainSettings& Settings,
	FAvidScriptEditorCommandLaunchConfig& InOutConfig)
{
	if (!Settings.Ldc2Path.IsEmpty())
	{
		InOutConfig.Ldc2Path = NormalizeAvidScriptEditorSettingsPath(Settings.Ldc2Path);
	}

	if (!Settings.ToolchainRoot.IsEmpty())
	{
		InOutConfig.ToolchainRoot = NormalizeAvidScriptEditorSettingsPath(Settings.ToolchainRoot);
	}

	const FString SourceId = GetAvidScriptEditorSettingsSourceId(InOutConfig);
	if (!Settings.OutputRoot.IsEmpty())
	{
		InOutConfig.OutputRoot = NormalizeAvidScriptEditorSettingsPath(FPaths::Combine(
			Settings.OutputRoot,
			SourceId));
	}

	if (!Settings.ReportRoot.IsEmpty())
	{
		InOutConfig.ReportPath = NormalizeAvidScriptEditorSettingsPath(FPaths::Combine(
			Settings.ReportRoot,
			SourceId + TEXT(".frontend.report.json")));
	}
}
