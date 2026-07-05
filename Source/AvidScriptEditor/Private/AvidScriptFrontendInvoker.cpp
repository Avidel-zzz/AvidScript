#include "AvidScriptFrontendInvoker.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
void NormalizeAvidScriptInvokerPath(FString& Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}
}

FString GetAvidScriptDefaultReportPath(const FString& SourcePath)
{
	FString BaseName = TEXT("frontend");
	if (!SourcePath.IsEmpty())
	{
		const FString SourceBaseName = FPaths::GetBaseFilename(SourcePath);
		if (!SourceBaseName.IsEmpty())
		{
			BaseName = SourceBaseName;
		}
	}

	FString ReportPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptReports"),
		BaseName + TEXT(".frontend.report.json"));
	ReportPath = FPaths::ConvertRelativePathToFull(ReportPath);
	FPaths::NormalizeFilename(ReportPath);
	return ReportPath;
}

void SetAvidScriptInvocationFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	FAvidScriptFrontendInvocationResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
}

FString QuoteAvidScriptPowerShellArgument(const FString& Value)
{
	FString EscapedValue = Value;
	EscapedValue.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
	return FString::Printf(TEXT("\"%s\""), *EscapedValue);
}

void AddAvidScriptPowerShellValueArgument(TArray<FString>& Arguments, const TCHAR* Name, const FString& Value)
{
	if (!Value.IsEmpty())
	{
		Arguments.Add(Name);
		Arguments.Add(QuoteAvidScriptPowerShellArgument(Value));
	}
}

FString BuildAvidScriptPowerShellParameters(const FAvidScriptFrontendInvocationConfig& Config)
{
	TArray<FString> Arguments;
	Arguments.Add(TEXT("-NoProfile"));
	Arguments.Add(TEXT("-ExecutionPolicy"));
	Arguments.Add(TEXT("Bypass"));
	Arguments.Add(TEXT("-File"));
	Arguments.Add(QuoteAvidScriptPowerShellArgument(Config.WrapperPath));
	AddAvidScriptPowerShellValueArgument(Arguments, TEXT("-SourcePath"), Config.SourcePath);
	AddAvidScriptPowerShellValueArgument(Arguments, TEXT("-BindingsPath"), Config.BindingsPath);
	AddAvidScriptPowerShellValueArgument(Arguments, TEXT("-OutputRoot"), Config.OutputRoot);
	AddAvidScriptPowerShellValueArgument(Arguments, TEXT("-ReportPath"), Config.ReportPath);
	AddAvidScriptPowerShellValueArgument(Arguments, TEXT("-Ldc2Path"), Config.Ldc2Path);
	AddAvidScriptPowerShellValueArgument(Arguments, TEXT("-ToolchainRoot"), Config.ToolchainRoot);

	if (Config.bSkipCompile)
	{
		Arguments.Add(TEXT("-SkipCompile"));
	}

	return FString::Join(Arguments, TEXT(" "));
}
} // namespace

FString FAvidScriptFrontendInvoker::GetDefaultWrapperPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (!Plugin.IsValid())
	{
		return FString();
	}

	FString WrapperPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Build"), TEXT("InvokeAvidScriptFrontend.ps1"));
	NormalizeAvidScriptInvokerPath(WrapperPath);
	return WrapperPath;
}

bool FAvidScriptFrontendInvoker::Invoke(
	const FAvidScriptFrontendInvocationConfig& Config,
	FAvidScriptFrontendInvocationResult& OutResult)
{
	OutResult = FAvidScriptFrontendInvocationResult();

#if PLATFORM_WINDOWS
	FAvidScriptFrontendInvocationConfig NormalizedConfig = Config;

	if (NormalizedConfig.WrapperPath.IsEmpty())
	{
		NormalizedConfig.WrapperPath = GetDefaultWrapperPath();
	}
	NormalizeAvidScriptInvokerPath(NormalizedConfig.WrapperPath);
	NormalizeAvidScriptInvokerPath(NormalizedConfig.SourcePath);
	NormalizeAvidScriptInvokerPath(NormalizedConfig.BindingsPath);
	NormalizeAvidScriptInvokerPath(NormalizedConfig.OutputRoot);
	NormalizeAvidScriptInvokerPath(NormalizedConfig.ReportPath);
	NormalizeAvidScriptInvokerPath(NormalizedConfig.Ldc2Path);
	NormalizeAvidScriptInvokerPath(NormalizedConfig.ToolchainRoot);

	if (NormalizedConfig.ReportPath.IsEmpty())
	{
		NormalizedConfig.ReportPath = GetAvidScriptDefaultReportPath(NormalizedConfig.SourcePath);
	}

	if (NormalizedConfig.WrapperPath.IsEmpty() || !FPaths::FileExists(NormalizedConfig.WrapperPath))
	{
		SetAvidScriptInvocationFailure(
			TEXT("wrapper_missing"),
			FString::Printf(TEXT("AvidScript frontend wrapper does not exist: %s"), *NormalizedConfig.WrapperPath),
			OutResult);
		return false;
	}

	if (NormalizedConfig.SourcePath.IsEmpty() || !FPaths::FileExists(NormalizedConfig.SourcePath))
	{
		SetAvidScriptInvocationFailure(
			TEXT("source_missing"),
			FString::Printf(TEXT("AvidScript source does not exist: %s"), *NormalizedConfig.SourcePath),
			OutResult);
		return false;
	}

	const FString ReportDirectory = FPaths::GetPath(NormalizedConfig.ReportPath);
	if (!ReportDirectory.IsEmpty() && !IFileManager::Get().MakeDirectory(*ReportDirectory, true))
	{
		SetAvidScriptInvocationFailure(
			TEXT("report_directory_failed"),
			FString::Printf(TEXT("AvidScript report directory could not be created: %s"), *ReportDirectory),
			OutResult);
		return false;
	}

	const FString Parameters = BuildAvidScriptPowerShellParameters(NormalizedConfig);
	const FString WorkingDirectory = FPaths::GetPath(NormalizedConfig.WrapperPath);
	const bool bProcessLaunched = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*Parameters,
		&OutResult.ProcessExitCode,
		&OutResult.Stdout,
		&OutResult.Stderr,
		*WorkingDirectory);

	if (!bProcessLaunched)
	{
		SetAvidScriptInvocationFailure(
			TEXT("process_failed"),
			FString::Printf(TEXT("AvidScript frontend process could not be launched: %s"), *NormalizedConfig.WrapperPath),
			OutResult);
		return false;
	}

	if (!FAvidScriptFrontendReportReader::LoadFromFile(
			NormalizedConfig.ReportPath,
			OutResult.Report,
			OutResult.ReportLoadResult))
	{
		SetAvidScriptInvocationFailure(
			OutResult.ReportLoadResult.ErrorCategory,
			OutResult.ReportLoadResult.ErrorMessage,
			OutResult);
		return false;
	}

	OutResult.bSucceeded =
		OutResult.ProcessExitCode == 0 &&
		OutResult.Report.bSucceeded &&
		!OutResult.Report.HasErrorDiagnostics();

	if (!OutResult.bSucceeded)
	{
		SetAvidScriptInvocationFailure(
			TEXT("frontend_failed"),
			FString::Printf(TEXT("AvidScript frontend failed with exit code %d"), OutResult.ProcessExitCode),
			OutResult);
		return false;
	}

	return true;
#else
	SetAvidScriptInvocationFailure(
		TEXT("platform_unsupported"),
		TEXT("AvidScript frontend invocation is currently implemented only for Windows Editor hosts."),
		OutResult);
	return false;
#endif
}
