#include "AvidScriptEditorCommandLauncher.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
FString NormalizeAvidScriptLauncherPath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}

	return Path;
}

FString GetAvidScriptLauncherSourceBaseName(const FString& SourcePath)
{
	const FString BaseName = FPaths::GetBaseFilename(SourcePath);
	return BaseName.IsEmpty() ? FString(TEXT("frontend")) : BaseName;
}

FString GetAvidScriptLauncherPluginBaseDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (!Plugin.IsValid())
	{
		return FString();
	}

	FString BaseDir = Plugin->GetBaseDir();
	BaseDir = FPaths::ConvertRelativePathToFull(BaseDir);
	FPaths::NormalizeFilename(BaseDir);
	return BaseDir;
}

FString MakeAvidScriptLauncherSummary(
	const FAvidScriptEditorCommandResult& CommandResult,
	const FAvidScriptEditorCommandLaunchResult& LaunchResult)
{
	if (!CommandResult.bSucceeded)
	{
		FString StdoutSnippet = CommandResult.CompileResult.InvocationResult.Stdout.Left(700);
		FString StderrSnippet = CommandResult.CompileResult.InvocationResult.Stderr.Left(700);
		StdoutSnippet.ReplaceInline(TEXT("\r"), TEXT(" "));
		StdoutSnippet.ReplaceInline(TEXT("\n"), TEXT(" "));
		StderrSnippet.ReplaceInline(TEXT("\r"), TEXT(" "));
		StderrSnippet.ReplaceInline(TEXT("\n"), TEXT(" "));
		return FString::Printf(
			TEXT("%s: %s next=%s exit=%d stdout=%s stderr=%s"),
			CommandResult.ErrorCategory.IsEmpty() ? TEXT("command_failed") : *CommandResult.ErrorCategory,
			*CommandResult.ErrorMessage,
			*CommandResult.NextAction,
			CommandResult.CompileResult.InvocationResult.ProcessExitCode,
			*StdoutSnippet,
			*StderrSnippet);
	}

	if (CommandResult.Status == EAvidScriptEditorCommandStatus::GeneratedOnly)
	{
		return FString::Printf(
			TEXT("generated: source=%s report=%s"),
			*LaunchResult.SourcePath,
			*LaunchResult.ReportPath);
	}

	return FString::Printf(
		TEXT("reload_applied: manifest=%s module=%s"),
		*LaunchResult.ManifestPath,
		*CommandResult.CompileResult.Manifest.ModuleId);
}

void SetAvidScriptLauncherFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	FAvidScriptEditorCommandLaunchResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.bReloadApplied = false;
	OutResult.Summary = FString::Printf(TEXT("%s: %s"), *ErrorCategory, *ErrorMessage);
	OutResult.CommandResult.ErrorCategory = ErrorCategory;
	OutResult.CommandResult.ErrorMessage = ErrorMessage;
}
} // namespace

bool FAvidScriptEditorCommandLauncher::MakeDefaultConfigForSource(
	const FString& SourcePath,
	FAvidScriptEditorCommandLaunchConfig& OutConfig,
	FString& OutErrorMessage)
{
	OutConfig = FAvidScriptEditorCommandLaunchConfig();
	OutErrorMessage.Reset();

	OutConfig.SourcePath = NormalizeAvidScriptLauncherPath(SourcePath);
	if (OutConfig.SourcePath.IsEmpty())
	{
		OutErrorMessage = TEXT("AvidScript source path is empty; choose a .avid source file.");
		return false;
	}

	if (!FPaths::FileExists(OutConfig.SourcePath))
	{
		OutErrorMessage = FString::Printf(TEXT("AvidScript source does not exist: %s"), *OutConfig.SourcePath);
		return false;
	}

	const FString PluginBaseDir = GetAvidScriptLauncherPluginBaseDir();
	if (PluginBaseDir.IsEmpty())
	{
		OutErrorMessage = TEXT("AvidScript plugin base directory could not be resolved.");
		return false;
	}

	const FString SourceBaseName = GetAvidScriptLauncherSourceBaseName(OutConfig.SourcePath);
	OutConfig.BindingsPath = NormalizeAvidScriptLauncherPath(FPaths::Combine(
		PluginBaseDir,
		TEXT("Bindings"),
		TEXT("ActorHostBindings.avidscript.json")));
	OutConfig.OutputRoot = NormalizeAvidScriptLauncherPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptGenerated"),
		SourceBaseName));
	OutConfig.ReportPath = NormalizeAvidScriptLauncherPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptReports"),
		SourceBaseName + TEXT(".frontend.report.json")));
	return true;
}

void FAvidScriptEditorCommandLauncher::SetHostContext(const FAvidScriptWasmHostContext& HostContext)
{
	ReloadSession.SetHostContext(HostContext);
}

const FAvidScriptWasmReloadSession& FAvidScriptEditorCommandLauncher::GetReloadSession() const
{
	return ReloadSession;
}

FAvidScriptWasmReloadSession& FAvidScriptEditorCommandLauncher::GetMutableReloadSession()
{
	return ReloadSession;
}

bool FAvidScriptEditorCommandLauncher::CompileSourceAndApply(
	const FAvidScriptEditorCommandLaunchConfig& Config,
	FAvidScriptEditorCommandLaunchResult& OutResult)
{
	OutResult = FAvidScriptEditorCommandLaunchResult();

	FAvidScriptEditorCommandLaunchConfig ResolvedConfig;
	FString ErrorMessage;
	if (!MakeDefaultConfigForSource(Config.SourcePath, ResolvedConfig, ErrorMessage))
	{
		OutResult.SourcePath = NormalizeAvidScriptLauncherPath(Config.SourcePath);
		SetAvidScriptLauncherFailure(TEXT("source_missing"), ErrorMessage, OutResult);
		return false;
	}

	if (!Config.BindingsPath.IsEmpty())
	{
		ResolvedConfig.BindingsPath = NormalizeAvidScriptLauncherPath(Config.BindingsPath);
	}
	if (!Config.OutputRoot.IsEmpty())
	{
		ResolvedConfig.OutputRoot = NormalizeAvidScriptLauncherPath(Config.OutputRoot);
	}
	if (!Config.ReportPath.IsEmpty())
	{
		ResolvedConfig.ReportPath = NormalizeAvidScriptLauncherPath(Config.ReportPath);
	}
	ResolvedConfig.Ldc2Path = NormalizeAvidScriptLauncherPath(Config.Ldc2Path);
	ResolvedConfig.ToolchainRoot = NormalizeAvidScriptLauncherPath(Config.ToolchainRoot);
	ResolvedConfig.bSkipCompile = Config.bSkipCompile;

	OutResult.SourcePath = ResolvedConfig.SourcePath;
	OutResult.BindingsPath = ResolvedConfig.BindingsPath;
	OutResult.OutputRoot = ResolvedConfig.OutputRoot;
	OutResult.ReportPath = ResolvedConfig.ReportPath;

	FAvidScriptEditorCommandConfig CommandConfig;
	CommandConfig.ReloadSession = &ReloadSession;
	CommandConfig.CompileConfig.InvocationConfig.SourcePath = ResolvedConfig.SourcePath;
	CommandConfig.CompileConfig.InvocationConfig.BindingsPath = ResolvedConfig.BindingsPath;
	CommandConfig.CompileConfig.InvocationConfig.OutputRoot = ResolvedConfig.OutputRoot;
	CommandConfig.CompileConfig.InvocationConfig.ReportPath = ResolvedConfig.ReportPath;
	CommandConfig.CompileConfig.InvocationConfig.Ldc2Path = ResolvedConfig.Ldc2Path;
	CommandConfig.CompileConfig.InvocationConfig.ToolchainRoot = ResolvedConfig.ToolchainRoot;
	CommandConfig.CompileConfig.InvocationConfig.bSkipCompile = ResolvedConfig.bSkipCompile;

	FAvidScriptEditorCommandResult CommandResult;
	const bool bCommandSucceeded = FAvidScriptEditorCommandService::CompileAndApply(CommandConfig, CommandResult);
	OutResult.CommandResult = CommandResult;
	OutResult.bSucceeded = bCommandSucceeded && CommandResult.bSucceeded;
	OutResult.bReloadApplied = CommandResult.bReloadApplied;
	OutResult.ManifestPath = CommandResult.CompileResult.ManifestPath;
	OutResult.Summary = MakeAvidScriptLauncherSummary(CommandResult, OutResult);
	return OutResult.bSucceeded;
}
