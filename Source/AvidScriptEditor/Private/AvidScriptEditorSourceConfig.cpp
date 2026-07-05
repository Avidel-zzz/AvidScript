#include "AvidScriptEditorSourceConfig.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
FString NormalizeAvidScriptSourceConfigPath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}

	return Path;
}

FString GetAvidScriptSourceConfigPluginBaseDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (Plugin.IsValid())
	{
		return NormalizeAvidScriptSourceConfigPath(Plugin->GetBaseDir());
	}

	const FString FallbackBaseDir = NormalizeAvidScriptSourceConfigPath(FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins"),
		TEXT("AvidScript")));
	return FPaths::DirectoryExists(FallbackBaseDir) ? FallbackBaseDir : FString();
}

FString MakeAvidScriptSourceConfigSourceId(const FString& SourcePath)
{
	const FString BaseName = FPaths::GetBaseFilename(SourcePath);
	return BaseName.IsEmpty() ? FString(TEXT("frontend")) : BaseName;
}

void SetAvidScriptSourceConfigFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorSourceConfigResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}
} // namespace

bool FAvidScriptEditorSourceConfigService::BuildLaunchConfig(
	const FAvidScriptEditorSourceConfigRequest& Request,
	FAvidScriptEditorSourceConfigResult& OutResult)
{
	OutResult = FAvidScriptEditorSourceConfigResult();

	if (Request.SourcePath.IsEmpty())
	{
		SetAvidScriptSourceConfigFailure(
			TEXT("source_empty"),
			TEXT("AvidScript source path is empty."),
			TEXT("choose a .avid source file"),
			OutResult);
		return false;
	}

	OutResult.NormalizedSourcePath = NormalizeAvidScriptSourceConfigPath(Request.SourcePath);
	if (!FPaths::GetExtension(OutResult.NormalizedSourcePath, true).Equals(TEXT(".avid"), ESearchCase::IgnoreCase))
	{
		SetAvidScriptSourceConfigFailure(
			TEXT("source_not_avid"),
			FString::Printf(TEXT("AvidScript source must use the .avid extension: %s"), *OutResult.NormalizedSourcePath),
			TEXT("choose a .avid source file"),
			OutResult);
		return false;
	}

	if (!FPaths::FileExists(OutResult.NormalizedSourcePath))
	{
		SetAvidScriptSourceConfigFailure(
			TEXT("source_missing"),
			FString::Printf(TEXT("AvidScript source does not exist: %s"), *OutResult.NormalizedSourcePath),
			TEXT("choose an existing .avid source file"),
			OutResult);
		return false;
	}

	const FString PluginBaseDir = GetAvidScriptSourceConfigPluginBaseDir();
	if (PluginBaseDir.IsEmpty())
	{
		SetAvidScriptSourceConfigFailure(
			TEXT("plugin_missing"),
			TEXT("AvidScript plugin base directory could not be resolved."),
			TEXT("verify the AvidScript plugin is installed under the project Plugins directory"),
			OutResult);
		return false;
	}

	OutResult.SourceId = MakeAvidScriptSourceConfigSourceId(OutResult.NormalizedSourcePath);
	OutResult.LaunchConfig.SourcePath = OutResult.NormalizedSourcePath;
	OutResult.LaunchConfig.BindingsPath = Request.BindingsPath.IsEmpty()
		? NormalizeAvidScriptSourceConfigPath(FPaths::Combine(
			PluginBaseDir,
			TEXT("Bindings"),
			TEXT("ActorHostBindings.avidscript.json")))
		: NormalizeAvidScriptSourceConfigPath(Request.BindingsPath);
	OutResult.LaunchConfig.OutputRoot = Request.OutputRoot.IsEmpty()
		? NormalizeAvidScriptSourceConfigPath(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScriptGenerated"),
			OutResult.SourceId))
		: NormalizeAvidScriptSourceConfigPath(Request.OutputRoot);
	OutResult.LaunchConfig.ReportPath = Request.ReportPath.IsEmpty()
		? NormalizeAvidScriptSourceConfigPath(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScriptReports"),
			OutResult.SourceId + TEXT(".frontend.report.json")))
		: NormalizeAvidScriptSourceConfigPath(Request.ReportPath);
	OutResult.bSucceeded = true;
	return true;
}
