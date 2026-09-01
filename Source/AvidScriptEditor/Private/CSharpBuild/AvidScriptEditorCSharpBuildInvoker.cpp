#include "CSharpBuild/AvidScriptEditorCSharpBuildInvoker.h"

#include "AvidScriptFrontendReport.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

namespace
{
void SetAvidScriptCSharpBuildInvocationFailure(
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

bool ApplyAvidScriptCSharpBuildReportMetadata(
	const FAvidScriptFrontendReport& Report,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	if (!Report.bHasToolInvocations
		|| !Report.bToolInvocationsValid
		|| Report.DiagnosticSchemaVersion != 1
		|| !Report.bHasSemanticCache
		|| !Report.bSemanticCacheValid
		|| !Report.bHasCompilationCache
		|| !Report.bCompilationCacheValid)
	{
		SetAvidScriptCSharpBuildInvocationFailure(
			TEXT("report_contract_invalid"),
			TEXT("C# build report is missing valid invocation or cache metadata."),
			TEXT("regenerate the C# build report with the current AvidScript toolchain"),
			OutResult);
		return false;
	}

	OutResult.FrontendInvocationCount = Report.FrontendInvocationCount;
	OutResult.SemanticInvocationCount = Report.SemanticInvocationCount;
	OutResult.GuestIrInvocationCount = Report.GuestIrInvocationCount;
	OutResult.WasmBackendInvocationCount = Report.WasmBackendInvocationCount;
	OutResult.SemanticCacheSchemaVersion = Report.SemanticCacheSchemaVersion;
	OutResult.bSemanticCacheEnabled = Report.bSemanticCacheEnabled;
	OutResult.SemanticCacheKey = Report.SemanticCacheKey;
	OutResult.SemanticCacheToolchainFingerprint = Report.SemanticCacheToolchainFingerprint;
	OutResult.SemanticCacheLookup = Report.SemanticCacheLookup;
	OutResult.SemanticCacheEntryReport = Report.SemanticCacheEntryReport;
	OutResult.SemanticCacheEntryReportSha256 = Report.SemanticCacheEntryReportSha256;
	OutResult.bSemanticCachePublished = Report.bSemanticCachePublished;
	OutResult.SemanticCacheDiagnosticCode = Report.SemanticCacheDiagnosticCode;
	OutResult.SemanticCacheDiagnosticMessage = Report.SemanticCacheDiagnosticMessage;
	OutResult.CompilationCacheSchemaVersion = Report.CompilationCacheSchemaVersion;
	OutResult.bCompilationCacheEnabled = Report.bCompilationCacheEnabled;
	OutResult.CompilationCacheKey = Report.CompilationCacheKey;
	OutResult.CompilationCacheToolchainFingerprint = Report.CompilationCacheToolchainFingerprint;
	OutResult.CompilationCacheLookup = Report.CompilationCacheLookup;
	OutResult.CompilationCacheEntryReport = Report.CompilationCacheEntryReport;
	OutResult.CompilationCacheEntryReportSha256 = Report.CompilationCacheEntryReportSha256;
	OutResult.bCompilationCachePublished = Report.bCompilationCachePublished;
	OutResult.CompilationCacheDiagnosticCode = Report.CompilationCacheDiagnosticCode;
	OutResult.CompilationCacheDiagnosticMessage = Report.CompilationCacheDiagnosticMessage;
	OutResult.Diagnostics = Report.Diagnostics;
	return true;
}

bool SetAvidScriptCSharpStructuredInvocationFailure(
	const FString& ReportPath,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult LoadResult;
	if (!FAvidScriptFrontendReportReader::LoadFromFile(ReportPath, Report, LoadResult) || Report.bSucceeded)
	{
		return false;
	}

	if (!ApplyAvidScriptCSharpBuildReportMetadata(Report, OutResult))
	{
		return true;
	}

	FString ErrorMessage;
	for (const FAvidScriptFrontendDiagnostic& Diagnostic : Report.Diagnostics)
	{
		if (Diagnostic.IsError())
		{
			const FString DiagnosticMessage = Diagnostic.Code.IsEmpty()
				? Diagnostic.Message
				: FString::Printf(TEXT("%s: %s"), *Diagnostic.Code, *Diagnostic.Message);
			ErrorMessage = Diagnostic.HasSourceLocation()
				? FString::Printf(
					TEXT("%s(%d,%d): %s"),
					*Diagnostic.File,
					Diagnostic.GetDisplayLine(),
					Diagnostic.GetDisplayColumn(),
					*DiagnosticMessage)
				: DiagnosticMessage;
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
		? FString(TEXT("generate the UE facade and binding descriptors before building this custom C# profile"))
		: FString(TEXT("fix the reported C# compiler diagnostic and rebuild the profile"));
	SetAvidScriptCSharpBuildInvocationFailure(ErrorCategory, ErrorMessage, NextAction, OutResult);
	return true;
}

FString QuoteAvidScriptCSharpBuildInvocationArgument(const FString& Value)
{
	FString EscapedValue = Value;
	EscapedValue.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
	return FString::Printf(TEXT("\"%s\""), *EscapedValue);
}

void AddAvidScriptCSharpBuildInvocationValueArgument(
	TArray<FString>& Arguments,
	const TCHAR* Name,
	const FString& Value)
{
	if (!Value.IsEmpty())
	{
		Arguments.Add(Name);
		Arguments.Add(QuoteAvidScriptCSharpBuildInvocationArgument(Value));
	}
}

FString BuildAvidScriptCSharpBuildInvocationParameters(const FAvidScriptEditorCSharpBuildConfig& Config)
{
	TArray<FString> Arguments;
	Arguments.Add(TEXT("-NoProfile"));
	Arguments.Add(TEXT("-ExecutionPolicy"));
	Arguments.Add(TEXT("Bypass"));
	Arguments.Add(TEXT("-File"));
	Arguments.Add(QuoteAvidScriptCSharpBuildInvocationArgument(Config.BuildScriptPath));
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-DotNetPath"), Config.DotNetPath);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-OutputRoot"), Config.OutputRoot);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-Configuration"), Config.Configuration);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-SourcePath"), Config.SourcePath);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-ProjectPath"), Config.ProjectPath);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-ModuleId"), Config.ModuleId);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-ArtifactStem"), Config.ArtifactStem);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-ReportPath"), Config.ReportPath);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-ManifestPath"), Config.ManifestPath);
	AddAvidScriptCSharpBuildInvocationValueArgument(
		Arguments,
		TEXT("-DataLaneFusion"),
		Config.bEnableDataLaneFusion ? TEXT("enabled") : TEXT("disabled"));
	AddAvidScriptCSharpBuildInvocationValueArgument(
		Arguments,
		TEXT("-PreparedBuildReportPath"),
		Config.PreparedBuildReportPath);
	AddAvidScriptCSharpBuildInvocationValueArgument(
		Arguments,
		TEXT("-SemanticCacheRoot"),
		Config.SemanticCacheRoot);
	AddAvidScriptCSharpBuildInvocationValueArgument(
		Arguments,
		TEXT("-CompilationCacheRoot"),
		Config.CompilationCacheRoot);
	AddAvidScriptCSharpBuildInvocationValueArgument(Arguments, TEXT("-BindingPackagePath"), Config.BindingPackagePath);
	AddAvidScriptCSharpBuildInvocationValueArgument(
		Arguments,
		TEXT("-RuntimeBindingPackagePath"),
		Config.RuntimeBindingPackagePath);
	if (Config.bOmitRuntimeBindingPackage)
	{
		Arguments.Add(TEXT("-OmitRuntimeBindingPackage"));
	}
	if (Config.bDisableSemanticCache)
	{
		Arguments.Add(TEXT("-DisableSemanticCache"));
	}
	if (Config.bDisableCompilationCache)
	{
		Arguments.Add(TEXT("-DisableCompilationCache"));
	}
	return FString::Join(Arguments, TEXT(" "));
}

bool MakeAvidScriptCSharpBuildInvocationDirectory(
	const FString& Directory,
	const FString& ErrorCategory,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, true))
	{
		SetAvidScriptCSharpBuildInvocationFailure(
			ErrorCategory,
			FString::Printf(TEXT("C# build directory could not be created: %s"), *Directory),
			TEXT("choose a writable C# output/report/manifest directory and retry"),
			OutResult);
		return false;
	}
	return true;
}

void InitializeAvidScriptCSharpBuildInvocationResult(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutResult = FAvidScriptEditorCSharpBuildResult();
	OutResult.SourcePath = Config.SourcePath;
	OutResult.ProjectPath = Config.ProjectPath;
	OutResult.BuildScriptPath = Config.BuildScriptPath;
	OutResult.OutputRoot = Config.OutputRoot;
	OutResult.ReportPath = Config.ReportPath;
	OutResult.ManifestPath = Config.ManifestPath;
	OutResult.AuthorizationBindingPackagePath = Config.BindingPackagePath;
	OutResult.BindingPackagePath = Config.bOmitRuntimeBindingPackage
		? FString()
		: (Config.RuntimeBindingPackagePath.IsEmpty()
			? Config.BindingPackagePath
			: Config.RuntimeBindingPackagePath);
	OutResult.ModuleId = Config.ModuleId;
	OutResult.ArtifactStem = Config.ArtifactStem;
}
} // namespace

bool FAvidScriptEditorCSharpBuildInvoker::Prepare(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildInvocation& OutInvocation,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutInvocation = FAvidScriptEditorCSharpBuildInvocation();
	InitializeAvidScriptCSharpBuildInvocationResult(Config, OutResult);

#if PLATFORM_WINDOWS
	if (!MakeAvidScriptCSharpBuildInvocationDirectory(Config.OutputRoot, TEXT("output_directory_failed"), OutResult)
		|| !MakeAvidScriptCSharpBuildInvocationDirectory(
			FPaths::GetPath(Config.ReportPath),
			TEXT("report_directory_failed"),
			OutResult)
		|| !MakeAvidScriptCSharpBuildInvocationDirectory(
			FPaths::GetPath(Config.ManifestPath),
			TEXT("manifest_directory_failed"),
			OutResult))
	{
		return false;
	}

	OutInvocation.Config = Config;
	OutInvocation.ExecutablePath = TEXT("pwsh.exe");
	OutInvocation.Parameters = BuildAvidScriptCSharpBuildInvocationParameters(Config);
	OutInvocation.WorkingDirectory = FPaths::GetPath(Config.BuildScriptPath);
	return true;
#else
	SetAvidScriptCSharpBuildInvocationFailure(
		TEXT("platform_unsupported"),
		TEXT("C# build invocation is currently implemented only for Windows Editor hosts."),
		TEXT("use a Windows Editor host for C# build-and-bind until other platforms are implemented"),
		OutResult);
	return false;
#endif
}

bool FAvidScriptEditorCSharpBuildInvoker::Finalize(
	const FAvidScriptEditorCSharpBuildInvocation& Invocation,
	const int32 ProcessExitCode,
	const FString& Stdout,
	const FString& Stderr,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	const FAvidScriptEditorCSharpBuildConfig& Config = Invocation.Config;
	InitializeAvidScriptCSharpBuildInvocationResult(Config, OutResult);
	OutResult.ProcessExitCode = ProcessExitCode;
	OutResult.Stdout = Stdout;
	OutResult.Stderr = Stderr;
	OutResult.BuildInvocationCount = 1;

	if (ProcessExitCode != 0)
	{
		if (FPaths::FileExists(Config.ReportPath)
			&& SetAvidScriptCSharpStructuredInvocationFailure(Config.ReportPath, OutResult))
		{
			return false;
		}
		SetAvidScriptCSharpBuildInvocationFailure(
			TEXT("build_failed"),
			FString::Printf(TEXT("C# build failed with exit code %d"), ProcessExitCode),
			TEXT("fix unsupported C# syntax or toolchain errors, then rerun Build And Bind C# Profile Script"),
			OutResult);
		return false;
	}

	if (!FPaths::FileExists(Config.ReportPath))
	{
		SetAvidScriptCSharpBuildInvocationFailure(
			TEXT("report_missing"),
			FString::Printf(TEXT("C# build report was not written: %s"), *Config.ReportPath),
			TEXT("check C# build stdout/stderr and rerun the build after the report can be written"),
			OutResult);
		return false;
	}

	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult ReportLoadResult;
	if (!FAvidScriptFrontendReportReader::LoadFromFile(Config.ReportPath, Report, ReportLoadResult))
	{
		SetAvidScriptCSharpBuildInvocationFailure(
			ReportLoadResult.ErrorCategory,
			ReportLoadResult.ErrorMessage,
			TEXT("repair the structured C# build report and rerun the profile build"),
			OutResult);
		return false;
	}
	if (!ApplyAvidScriptCSharpBuildReportMetadata(Report, OutResult))
	{
		return false;
	}
	if (!Report.bSucceeded)
	{
		if (!SetAvidScriptCSharpStructuredInvocationFailure(Config.ReportPath, OutResult))
		{
			SetAvidScriptCSharpBuildInvocationFailure(
				TEXT("report_failed"),
				TEXT("C# build process exited successfully but the structured report records failure."),
				TEXT("fix the reported C# compiler diagnostic and rebuild the profile"),
				OutResult);
		}
		return false;
	}
	if (Report.SchemaVersion != 1 || Report.Result != TEXT("direct_abi_built") || Report.HasErrorDiagnostics())
	{
		SetAvidScriptCSharpBuildInvocationFailure(
			TEXT("report_contract_invalid"),
			TEXT("C# build process exited successfully but the structured report success contract is invalid."),
			TEXT("regenerate the C# build report with the current AvidScript toolchain"),
			OutResult);
		return false;
	}
	if (!FPaths::FileExists(Config.ManifestPath))
	{
		SetAvidScriptCSharpBuildInvocationFailure(
			TEXT("manifest_missing"),
			FString::Printf(TEXT("C# build manifest was not written: %s"), *Config.ManifestPath),
			TEXT("inspect build publication diagnostics and rebuild the C# profile"),
			OutResult);
		return false;
	}

	const FString WasmPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		Config.OutputRoot,
		Config.ArtifactStem + TEXT(".wasm")));
	if (!FPaths::FileExists(WasmPath))
	{
		SetAvidScriptCSharpBuildInvocationFailure(
			TEXT("wasm_missing"),
			FString::Printf(TEXT("C# build WASM was not written: %s"), *WasmPath),
			TEXT("inspect Guest IR/backend diagnostics and rebuild the C# profile"),
			OutResult);
		return false;
	}

	OutResult.bSucceeded = true;
	return true;
}

bool FAvidScriptEditorCSharpBuildInvoker::BuildOnce(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	FAvidScriptEditorCSharpBuildInvocation Invocation;
	if (!Prepare(Config, Invocation, OutResult))
	{
		return false;
	}

	int32 ProcessExitCode = INDEX_NONE;
	FString Stdout;
	FString Stderr;
	const bool bProcessLaunched = FPlatformProcess::ExecProcess(
		*Invocation.ExecutablePath,
		*Invocation.Parameters,
		&ProcessExitCode,
		&Stdout,
		&Stderr,
		*Invocation.WorkingDirectory);
	if (!bProcessLaunched)
	{
		SetAvidScriptCSharpBuildInvocationFailure(
			TEXT("process_failed"),
			FString::Printf(TEXT("C# build process could not be launched: %s"), *Config.BuildScriptPath),
			TEXT("verify PowerShell 7 (pwsh.exe) can run the C# build script and retry"),
			OutResult);
		return false;
	}
	return Finalize(Invocation, ProcessExitCode, Stdout, Stderr, OutResult);
}
