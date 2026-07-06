#include "AvidScriptEditorCSharpBuildService.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
void NormalizeAvidScriptCSharpBuildPath(FString& Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}
}

FString NormalizeAvidScriptCSharpBuildPathCopy(FString Path)
{
	NormalizeAvidScriptCSharpBuildPath(Path);
	return Path;
}

FString GetAvidScriptCSharpBuildPluginBaseDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (Plugin.IsValid())
	{
		return NormalizeAvidScriptCSharpBuildPathCopy(Plugin->GetBaseDir());
	}

	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins"),
		TEXT("AvidScript")));
}

FString MakeAvidScriptCSharpReportPathForOutputRoot(const FString& OutputRoot)
{
	FString ReportPath = FPaths::Combine(OutputRoot, TEXT("actor_lifecycle.csharp.report.json"));
	NormalizeAvidScriptCSharpBuildPath(ReportPath);
	return ReportPath;
}

void SetAvidScriptCSharpBuildFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
}

FString QuoteAvidScriptCSharpPowerShellArgument(const FString& Value)
{
	FString EscapedValue = Value;
	EscapedValue.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
	return FString::Printf(TEXT("\"%s\""), *EscapedValue);
}

void AddAvidScriptCSharpPowerShellValueArgument(TArray<FString>& Arguments, const TCHAR* Name, const FString& Value)
{
	if (!Value.IsEmpty())
	{
		Arguments.Add(Name);
		Arguments.Add(QuoteAvidScriptCSharpPowerShellArgument(Value));
	}
}

FString BuildAvidScriptCSharpPowerShellParameters(const FAvidScriptEditorCSharpBuildConfig& Config)
{
	TArray<FString> Arguments;
	Arguments.Add(TEXT("-NoProfile"));
	Arguments.Add(TEXT("-ExecutionPolicy"));
	Arguments.Add(TEXT("Bypass"));
	Arguments.Add(TEXT("-File"));
	Arguments.Add(QuoteAvidScriptCSharpPowerShellArgument(Config.BuildScriptPath));
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-DotNetPath"), Config.DotNetPath);
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-OutputRoot"), Config.OutputRoot);
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-Configuration"), Config.Configuration);
	return FString::Join(Arguments, TEXT(" "));
}
} // namespace

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		GetAvidScriptCSharpBuildPluginBaseDir(),
		TEXT("Build"),
		TEXT("BuildCSharpActorLifecycle.ps1")));
}

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleOutputRoot()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ActorLifecycle")));
}

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleReportPath()
{
	return MakeAvidScriptCSharpReportPathForOutputRoot(GetDefaultActorLifecycleOutputRoot());
}

bool FAvidScriptEditorCSharpBuildService::BuildActorLifecycle(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutResult = FAvidScriptEditorCSharpBuildResult();

#if PLATFORM_WINDOWS
	FAvidScriptEditorCSharpBuildConfig NormalizedConfig = Config;
	if (NormalizedConfig.BuildScriptPath.IsEmpty())
	{
		NormalizedConfig.BuildScriptPath = GetDefaultActorLifecycleBuildScriptPath();
	}
	if (NormalizedConfig.OutputRoot.IsEmpty())
	{
		NormalizedConfig.OutputRoot = GetDefaultActorLifecycleOutputRoot();
	}
	if (NormalizedConfig.ReportPath.IsEmpty())
	{
		NormalizedConfig.ReportPath = MakeAvidScriptCSharpReportPathForOutputRoot(NormalizedConfig.OutputRoot);
	}
	if (NormalizedConfig.Configuration.IsEmpty())
	{
		NormalizedConfig.Configuration = TEXT("Release");
	}

	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.BuildScriptPath);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.OutputRoot);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.ReportPath);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.DotNetPath);

	OutResult.BuildScriptPath = NormalizedConfig.BuildScriptPath;
	OutResult.OutputRoot = NormalizedConfig.OutputRoot;
	OutResult.ReportPath = NormalizedConfig.ReportPath;

	if (NormalizedConfig.BuildScriptPath.IsEmpty() || !FPaths::FileExists(NormalizedConfig.BuildScriptPath))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("build_script_missing"),
			FString::Printf(TEXT("C# ActorLifecycle build script does not exist: %s"), *NormalizedConfig.BuildScriptPath),
			OutResult);
		return false;
	}

	if (!IFileManager::Get().MakeDirectory(*NormalizedConfig.OutputRoot, true))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("output_directory_failed"),
			FString::Printf(TEXT("C# ActorLifecycle output directory could not be created: %s"), *NormalizedConfig.OutputRoot),
			OutResult);
		return false;
	}

	const FString Parameters = BuildAvidScriptCSharpPowerShellParameters(NormalizedConfig);
	const FString WorkingDirectory = FPaths::GetPath(NormalizedConfig.BuildScriptPath);
	const bool bProcessLaunched = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*Parameters,
		&OutResult.ProcessExitCode,
		&OutResult.Stdout,
		&OutResult.Stderr,
		*WorkingDirectory);

	if (!bProcessLaunched)
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("process_failed"),
			FString::Printf(TEXT("C# ActorLifecycle build process could not be launched: %s"), *NormalizedConfig.BuildScriptPath),
			OutResult);
		return false;
	}

	if (OutResult.ProcessExitCode != 0)
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("build_failed"),
			FString::Printf(TEXT("C# ActorLifecycle build failed with exit code %d"), OutResult.ProcessExitCode),
			OutResult);
		return false;
	}

	if (!FPaths::FileExists(NormalizedConfig.ReportPath))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("report_missing"),
			FString::Printf(TEXT("C# ActorLifecycle build report was not written: %s"), *NormalizedConfig.ReportPath),
			OutResult);
		return false;
	}

	OutResult.bSucceeded = true;
	return true;
#else
	SetAvidScriptCSharpBuildFailure(
		TEXT("platform_unsupported"),
		TEXT("C# ActorLifecycle build invocation is currently implemented only for Windows Editor hosts."),
		OutResult);
	return false;
#endif
}
