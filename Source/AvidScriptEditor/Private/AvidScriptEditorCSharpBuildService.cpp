#include "AvidScriptEditorCSharpBuildService.h"

#include "AvidScriptFrontendReport.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
constexpr const TCHAR* AvidScriptDefaultCSharpActorLifecycleModuleId = TEXT("csharp_actor_lifecycle");
constexpr const TCHAR* AvidScriptDefaultCSharpActorLifecycleArtifactStem = TEXT("actor_lifecycle");

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

FString GetAvidScriptCSharpArtifactStemOrDefault(const FString& ArtifactStem)
{
	return ArtifactStem.IsEmpty()
		? FString(AvidScriptDefaultCSharpActorLifecycleArtifactStem)
		: ArtifactStem;
}

FString MakeAvidScriptCSharpReportPathForOutputRoot(const FString& OutputRoot, const FString& ArtifactStem)
{
	FString ReportPath = FPaths::Combine(
		OutputRoot,
		GetAvidScriptCSharpArtifactStemOrDefault(ArtifactStem) + TEXT(".csharp.report.json"));
	NormalizeAvidScriptCSharpBuildPath(ReportPath);
	return ReportPath;
}

FString MakeAvidScriptCSharpManifestPathForOutputRoot(const FString& OutputRoot, const FString& ArtifactStem)
{
	FString ManifestPath = FPaths::Combine(
		OutputRoot,
		GetAvidScriptCSharpArtifactStemOrDefault(ArtifactStem) + TEXT(".avidscript.json"));
	NormalizeAvidScriptCSharpBuildPath(ManifestPath);
	return ManifestPath;
}

void SetAvidScriptCSharpBuildFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}

bool SetAvidScriptCSharpStructuredBuildFailure(
	const FString& ReportPath,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult LoadResult;
	if (!FAvidScriptFrontendReportReader::LoadFromFile(ReportPath, Report, LoadResult) || Report.bSucceeded)
	{
		return false;
	}

	FString ErrorMessage;
	for (const FAvidScriptFrontendDiagnostic& Diagnostic : Report.Diagnostics)
	{
		if (Diagnostic.IsError())
		{
			ErrorMessage = Diagnostic.Code.IsEmpty()
				? Diagnostic.Message
				: FString::Printf(TEXT("%s: %s"), *Diagnostic.Code, *Diagnostic.Message);
			break;
		}
	}
	if (ErrorMessage.IsEmpty())
	{
		ErrorMessage = TEXT("C# build failed; inspect the structured build report for diagnostics.");
	}

	const FString ErrorCategory = Report.Result.IsEmpty()
		? FString(TEXT("build_failed"))
		: Report.Result;
	const FString NextAction = ErrorCategory == TEXT("phase42_binding_required")
		? FString(TEXT("generate the Phase 42 UE facade and binding descriptors before building this custom C# profile"))
		: FString(TEXT("fix the reported C# compiler diagnostic and rebuild the profile"));
	SetAvidScriptCSharpBuildFailure(ErrorCategory, ErrorMessage, NextAction, OutResult);
	return true;
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
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-SourcePath"), Config.SourcePath);
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-ProjectPath"), Config.ProjectPath);
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-ModuleId"), Config.ModuleId);
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-ArtifactStem"), Config.ArtifactStem);
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-ReportPath"), Config.ReportPath);
	AddAvidScriptCSharpPowerShellValueArgument(Arguments, TEXT("-ManifestPath"), Config.ManifestPath);
	return FString::Join(Arguments, TEXT(" "));
}

bool MakeAvidScriptCSharpBuildDirectory(const FString& Directory, const FString& ErrorCategory, FAvidScriptEditorCSharpBuildResult& OutResult)
{
	if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, true))
	{
		SetAvidScriptCSharpBuildFailure(
			ErrorCategory,
			FString::Printf(TEXT("C# build directory could not be created: %s"), *Directory),
			TEXT("choose a writable C# output/report/manifest directory and retry"),
			OutResult);
		return false;
	}

	return true;
}
} // namespace

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		GetAvidScriptCSharpBuildPluginBaseDir(),
		TEXT("Build"),
		TEXT("BuildCSharpActorLifecycle.ps1")));
}

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		GetAvidScriptCSharpBuildPluginBaseDir(),
		TEXT("Samples"),
		TEXT("CSharp"),
		TEXT("ActorLifecycle"),
		TEXT("ActorLifecycleScript.cs")));
}

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		GetAvidScriptCSharpBuildPluginBaseDir(),
		TEXT("Samples"),
		TEXT("CSharp"),
		TEXT("ActorLifecycle"),
		TEXT("AvidScript.ActorLifecycle.csproj")));
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
	return MakeReportPathForOutputRoot(GetDefaultActorLifecycleOutputRoot(), GetDefaultActorLifecycleArtifactStem());
}

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleManifestPath()
{
	return MakeManifestPathForOutputRoot(GetDefaultActorLifecycleOutputRoot(), GetDefaultActorLifecycleArtifactStem());
}

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleModuleId()
{
	return AvidScriptDefaultCSharpActorLifecycleModuleId;
}

FString FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleArtifactStem()
{
	return AvidScriptDefaultCSharpActorLifecycleArtifactStem;
}

FString FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(const FString& OutputRoot, const FString& ArtifactStem)
{
	return MakeAvidScriptCSharpReportPathForOutputRoot(OutputRoot, ArtifactStem);
}

FString FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(const FString& OutputRoot, const FString& ArtifactStem)
{
	return MakeAvidScriptCSharpManifestPathForOutputRoot(OutputRoot, ArtifactStem);
}

bool FAvidScriptEditorCSharpBuildService::BuildProfile(
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
	if (NormalizedConfig.SourcePath.IsEmpty())
	{
		NormalizedConfig.SourcePath = GetDefaultActorLifecycleSourcePath();
	}
	if (NormalizedConfig.ProjectPath.IsEmpty())
	{
		NormalizedConfig.ProjectPath = GetDefaultActorLifecycleProjectPath();
	}
	if (NormalizedConfig.OutputRoot.IsEmpty())
	{
		NormalizedConfig.OutputRoot = GetDefaultActorLifecycleOutputRoot();
	}
	if (NormalizedConfig.ModuleId.IsEmpty())
	{
		NormalizedConfig.ModuleId = GetDefaultActorLifecycleModuleId();
	}
	if (NormalizedConfig.ArtifactStem.IsEmpty())
	{
		NormalizedConfig.ArtifactStem = GetDefaultActorLifecycleArtifactStem();
	}
	if (NormalizedConfig.ReportPath.IsEmpty())
	{
		NormalizedConfig.ReportPath = MakeReportPathForOutputRoot(NormalizedConfig.OutputRoot, NormalizedConfig.ArtifactStem);
	}
	if (NormalizedConfig.ManifestPath.IsEmpty())
	{
		NormalizedConfig.ManifestPath = MakeManifestPathForOutputRoot(NormalizedConfig.OutputRoot, NormalizedConfig.ArtifactStem);
	}
	if (NormalizedConfig.Configuration.IsEmpty())
	{
		NormalizedConfig.Configuration = TEXT("Release");
	}

	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.BuildScriptPath);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.SourcePath);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.ProjectPath);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.OutputRoot);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.ReportPath);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.ManifestPath);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.DotNetPath);

	OutResult.SourcePath = NormalizedConfig.SourcePath;
	OutResult.ProjectPath = NormalizedConfig.ProjectPath;
	OutResult.BuildScriptPath = NormalizedConfig.BuildScriptPath;
	OutResult.OutputRoot = NormalizedConfig.OutputRoot;
	OutResult.ReportPath = NormalizedConfig.ReportPath;
	OutResult.ManifestPath = NormalizedConfig.ManifestPath;
	OutResult.ModuleId = NormalizedConfig.ModuleId;
	OutResult.ArtifactStem = NormalizedConfig.ArtifactStem;

	if (NormalizedConfig.BuildScriptPath.IsEmpty() || !FPaths::FileExists(NormalizedConfig.BuildScriptPath))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("build_script_missing"),
			FString::Printf(TEXT("C# build script does not exist: %s"), *NormalizedConfig.BuildScriptPath),
			TEXT("verify BuildCSharpActorLifecycle.ps1 exists in the plugin Build directory"),
			OutResult);
		return false;
	}

	if (NormalizedConfig.SourcePath.IsEmpty() || !FPaths::FileExists(NormalizedConfig.SourcePath))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("source_missing"),
			FString::Printf(TEXT("C# source file does not exist: %s"), *NormalizedConfig.SourcePath),
			TEXT("choose an existing C# source file or regenerate the default C# profile"),
			OutResult);
		return false;
	}

	if (!NormalizedConfig.ProjectPath.IsEmpty() && !FPaths::FileExists(NormalizedConfig.ProjectPath))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("project_missing"),
			FString::Printf(TEXT("C# project file does not exist: %s"), *NormalizedConfig.ProjectPath),
			TEXT("choose an existing C# project file or update project_path in the C# profile"),
			OutResult);
		return false;
	}

	if (!MakeAvidScriptCSharpBuildDirectory(NormalizedConfig.OutputRoot, TEXT("output_directory_failed"), OutResult) ||
		!MakeAvidScriptCSharpBuildDirectory(FPaths::GetPath(NormalizedConfig.ReportPath), TEXT("report_directory_failed"), OutResult) ||
		!MakeAvidScriptCSharpBuildDirectory(FPaths::GetPath(NormalizedConfig.ManifestPath), TEXT("manifest_directory_failed"), OutResult))
	{
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
			FString::Printf(TEXT("C# build process could not be launched: %s"), *NormalizedConfig.BuildScriptPath),
			TEXT("verify powershell.exe can run the C# build script and retry"),
			OutResult);
		return false;
	}

	if (OutResult.ProcessExitCode != 0)
	{
		if (FPaths::FileExists(NormalizedConfig.ReportPath) &&
			SetAvidScriptCSharpStructuredBuildFailure(NormalizedConfig.ReportPath, OutResult))
		{
			return false;
		}

		SetAvidScriptCSharpBuildFailure(
			TEXT("build_failed"),
			FString::Printf(TEXT("C# build failed with exit code %d"), OutResult.ProcessExitCode),
			TEXT("fix unsupported C# syntax or toolchain errors, then rerun Build And Bind C# Profile Script"),
			OutResult);
		return false;
	}

	if (!FPaths::FileExists(NormalizedConfig.ReportPath))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("report_missing"),
			FString::Printf(TEXT("C# build report was not written: %s"), *NormalizedConfig.ReportPath),
			TEXT("check C# build stdout/stderr and rerun the build after the report can be written"),
			OutResult);
		return false;
	}

	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult ReportLoadResult;
	if (!FAvidScriptFrontendReportReader::LoadFromFile(
		NormalizedConfig.ReportPath,
		Report,
		ReportLoadResult))
	{
		SetAvidScriptCSharpBuildFailure(
			ReportLoadResult.ErrorCategory,
			ReportLoadResult.ErrorMessage,
			TEXT("repair the structured C# build report and rerun the profile build"),
			OutResult);
		return false;
	}

	if (!Report.bSucceeded)
	{
		if (!SetAvidScriptCSharpStructuredBuildFailure(NormalizedConfig.ReportPath, OutResult))
		{
			SetAvidScriptCSharpBuildFailure(
				TEXT("report_failed"),
				TEXT("C# build process exited successfully but the structured report records failure."),
				TEXT("fix the reported C# compiler diagnostic and rebuild the profile"),
				OutResult);
		}
		return false;
	}

	if (Report.SchemaVersion != 1 || Report.Result != TEXT("direct_abi_built") || Report.HasErrorDiagnostics())
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("report_contract_invalid"),
			TEXT("C# build process exited successfully but the structured report success contract is invalid."),
			TEXT("regenerate the C# build report with the current AvidScript toolchain"),
			OutResult);
		return false;
	}

	if (!FPaths::FileExists(NormalizedConfig.ManifestPath))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("manifest_missing"),
			FString::Printf(TEXT("C# build manifest was not written: %s"), *NormalizedConfig.ManifestPath),
			TEXT("inspect build publication diagnostics and rebuild the C# profile"),
			OutResult);
		return false;
	}

	const FString WasmPath = NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		NormalizedConfig.OutputRoot,
		NormalizedConfig.ArtifactStem + TEXT(".wasm")));
	if (!FPaths::FileExists(WasmPath))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("wasm_missing"),
			FString::Printf(TEXT("C# build WASM was not written: %s"), *WasmPath),
			TEXT("inspect Guest IR/backend diagnostics and rebuild the C# profile"),
			OutResult);
		return false;
	}

	OutResult.bSucceeded = true;
	return true;
#else
	SetAvidScriptCSharpBuildFailure(
		TEXT("platform_unsupported"),
		TEXT("C# build invocation is currently implemented only for Windows Editor hosts."),
		TEXT("use a Windows Editor host for C# build-and-bind until other platforms are implemented"),
		OutResult);
	return false;
#endif
}

bool FAvidScriptEditorCSharpBuildService::BuildActorLifecycle(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	return BuildProfile(Config, OutResult);
}
