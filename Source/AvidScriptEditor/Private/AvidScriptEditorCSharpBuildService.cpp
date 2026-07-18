#include "AvidScriptEditorCSharpBuildService.h"

#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptFrontendReport.h"
#include "CSharpBuild/AvidScriptEditorCSharpBindingSliceService.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildInvoker.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

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

struct FAvidScriptCSharpBuildInvocationCounts
{
	int32 Build = 0;
	int32 Frontend = 0;
	int32 Semantic = 0;
	int32 GuestIr = 0;
	int32 WasmBackend = 0;

	static FAvidScriptCSharpBuildInvocationCounts FromResult(
		const FAvidScriptEditorCSharpBuildResult& Result)
	{
		return {
			Result.BuildInvocationCount,
			Result.FrontendInvocationCount,
			Result.SemanticInvocationCount,
			Result.GuestIrInvocationCount,
			Result.WasmBackendInvocationCount
		};
	}
};

void SetAvidScriptCSharpBuildResultMetadata(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	const FString& AuthorizationBindingPackagePath,
	const FString& RuntimeBindingPackagePath,
	const FAvidScriptCSharpBuildInvocationCounts& InvocationCounts,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutResult.SourcePath = Config.SourcePath;
	OutResult.ProjectPath = Config.ProjectPath;
	OutResult.BuildScriptPath = Config.BuildScriptPath;
	OutResult.OutputRoot = Config.OutputRoot;
	OutResult.ReportPath = Config.ReportPath;
	OutResult.ManifestPath = Config.ManifestPath;
	OutResult.AuthorizationBindingPackagePath = AuthorizationBindingPackagePath;
	OutResult.BindingPackagePath = RuntimeBindingPackagePath;
	OutResult.ModuleId = Config.ModuleId;
	OutResult.ArtifactStem = Config.ArtifactStem;
	OutResult.BuildInvocationCount = InvocationCounts.Build;
	OutResult.FrontendInvocationCount = InvocationCounts.Frontend;
	OutResult.SemanticInvocationCount = InvocationCounts.Semantic;
	OutResult.GuestIrInvocationCount = InvocationCounts.GuestIr;
	OutResult.WasmBackendInvocationCount = InvocationCounts.WasmBackend;
}

void ApplyAvidScriptCSharpBuildInvocationOutcome(
	const FAvidScriptEditorCSharpBuildResult& InvocationResult,
	const FAvidScriptEditorCSharpBuildConfig& FinalConfig,
	const FString& AuthorizationBindingPackagePath,
	const FString& RuntimeBindingPackagePath,
	const FAvidScriptCSharpBuildInvocationCounts& InvocationCounts,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutResult = InvocationResult;
	SetAvidScriptCSharpBuildResultMetadata(
		FinalConfig,
		AuthorizationBindingPackagePath,
		RuntimeBindingPackagePath,
		InvocationCounts,
		OutResult);
}

void CopyAvidScriptCSharpSemanticCacheAudit(
	const FAvidScriptEditorCSharpBuildResult& Source,
	FAvidScriptEditorCSharpBuildResult& Destination)
{
	Destination.SemanticCacheSchemaVersion = Source.SemanticCacheSchemaVersion;
	Destination.bSemanticCacheEnabled = Source.bSemanticCacheEnabled;
	Destination.SemanticCacheKey = Source.SemanticCacheKey;
	Destination.SemanticCacheToolchainFingerprint = Source.SemanticCacheToolchainFingerprint;
	Destination.SemanticCacheLookup = Source.SemanticCacheLookup;
	Destination.SemanticCacheEntryReport = Source.SemanticCacheEntryReport;
	Destination.SemanticCacheEntryReportSha256 = Source.SemanticCacheEntryReportSha256;
	Destination.bSemanticCachePublished = Source.bSemanticCachePublished;
	Destination.SemanticCacheDiagnosticCode = Source.SemanticCacheDiagnosticCode;
	Destination.SemanticCacheDiagnosticMessage = Source.SemanticCacheDiagnosticMessage;
}

void RemoveAvidScriptCSharpFinalLoadableArtifacts(const FAvidScriptEditorCSharpBuildConfig& Config)
{
	const TArray<FString> Artifacts = {
		Config.ReportPath,
		Config.ManifestPath,
		FPaths::Combine(Config.OutputRoot, Config.ArtifactStem + TEXT(".wasm"))
	};
	for (const FString& Artifact : Artifacts)
	{
		if (!Artifact.IsEmpty())
		{
			IFileManager::Get().Delete(*Artifact, false, true, true);
		}
	}
}

FString MakeAvidScriptCSharpBootstrapRoot()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("AvidScript"),
		TEXT("CSharpBootstrap"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

bool ValidateAvidScriptCSharpBuildPackagePath(
	const FString& PackagePath,
	const FString& ErrorCategory,
	const FString& Description,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	if (PackagePath.IsEmpty() || FPaths::FileExists(PackagePath))
	{
		return true;
	}

	SetAvidScriptCSharpBuildFailure(
		ErrorCategory,
		FString::Printf(TEXT("%s does not exist: %s"), *Description, *PackagePath),
		TEXT("publish a generated binding package or update the profile package path"),
		OutResult);
	return false;
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

FString FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
	const FString& OutputRoot,
	const FString& ArtifactStem)
{
	return MakeAvidScriptCSharpReportPathForOutputRoot(OutputRoot, ArtifactStem);
}

FString FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
	const FString& OutputRoot,
	const FString& ArtifactStem)
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
		NormalizedConfig.ReportPath = MakeReportPathForOutputRoot(
			NormalizedConfig.OutputRoot,
			NormalizedConfig.ArtifactStem);
	}
	if (NormalizedConfig.ManifestPath.IsEmpty())
	{
		NormalizedConfig.ManifestPath = MakeManifestPathForOutputRoot(
			NormalizedConfig.OutputRoot,
			NormalizedConfig.ArtifactStem);
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
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.SemanticCacheRoot);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.BindingPackagePath);
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.RuntimeBindingPackagePath);
	NormalizedConfig.PreparedBuildReportPath.Reset();
	NormalizeAvidScriptCSharpBuildPath(NormalizedConfig.DotNetPath);

	FString AuthorizationBindingPackagePath = NormalizedConfig.BindingPackagePath;
	FString RuntimeBindingPackagePath = NormalizedConfig.bOmitRuntimeBindingPackage
		? FString()
		: NormalizedConfig.RuntimeBindingPackagePath;
	SetAvidScriptCSharpBuildResultMetadata(
		NormalizedConfig,
		AuthorizationBindingPackagePath,
		RuntimeBindingPackagePath,
		FAvidScriptCSharpBuildInvocationCounts(),
		OutResult);

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

	const bool bRequiresGeneratedBindingPackage = !NormalizedConfig.SourcePath.Equals(
		GetDefaultActorLifecycleSourcePath(),
		ESearchCase::IgnoreCase);
	const bool bAutomaticBindingSlice = bRequiresGeneratedBindingPackage
		&& AuthorizationBindingPackagePath.IsEmpty();
	if (bAutomaticBindingSlice
		&& (!NormalizedConfig.RuntimeBindingPackagePath.IsEmpty()
			|| NormalizedConfig.bOmitRuntimeBindingPackage))
	{
		SetAvidScriptCSharpBuildFailure(
			TEXT("binding_package_strategy_invalid"),
			TEXT("Runtime package overrides require an explicit authorization BindingPackagePath."),
			TEXT("set BindingPackagePath or clear runtime package overrides to use automatic slicing"),
			OutResult);
		return false;
	}

	if (bAutomaticBindingSlice)
	{
		FAvidScriptCSharpBindingEmitResult BindingEmitResult;
		if (!FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(BindingEmitResult))
		{
			SetAvidScriptCSharpBuildFailure(
				BindingEmitResult.ErrorCategory.IsEmpty()
					? FString(TEXT("binding_package_publish_failed"))
					: BindingEmitResult.ErrorCategory,
				BindingEmitResult.ErrorMessage.IsEmpty()
					? FString(TEXT("Engine gameplay C# binding package could not be published."))
					: BindingEmitResult.ErrorMessage,
				BindingEmitResult.NextAction.IsEmpty()
					? FString(TEXT("repair the reflected binding selection and retry the custom C# build"))
					: BindingEmitResult.NextAction,
				OutResult);
			return false;
		}
		AuthorizationBindingPackagePath = NormalizeAvidScriptCSharpBuildPathCopy(
			BindingEmitResult.ManifestPath);
		NormalizedConfig.BindingPackagePath = AuthorizationBindingPackagePath;
		SetAvidScriptCSharpBuildResultMetadata(
			NormalizedConfig,
			AuthorizationBindingPackagePath,
			FString(),
			FAvidScriptCSharpBuildInvocationCounts(),
			OutResult);
	}

	if (!ValidateAvidScriptCSharpBuildPackagePath(
			AuthorizationBindingPackagePath,
			TEXT("binding_package_missing"),
			TEXT("C# authorization binding package manifest"),
			OutResult)
		|| !ValidateAvidScriptCSharpBuildPackagePath(
			NormalizedConfig.RuntimeBindingPackagePath,
			TEXT("runtime_binding_package_missing"),
			TEXT("C# runtime binding package manifest"),
			OutResult))
	{
		return false;
	}

	if (!bAutomaticBindingSlice)
	{
		if (!NormalizedConfig.bOmitRuntimeBindingPackage && RuntimeBindingPackagePath.IsEmpty())
		{
			RuntimeBindingPackagePath = AuthorizationBindingPackagePath;
		}
		FAvidScriptEditorCSharpBuildResult InvocationResult;
		const bool bBuildSucceeded = FAvidScriptEditorCSharpBuildInvoker::BuildOnce(
			NormalizedConfig,
			InvocationResult);
		ApplyAvidScriptCSharpBuildInvocationOutcome(
			InvocationResult,
			NormalizedConfig,
			AuthorizationBindingPackagePath,
			RuntimeBindingPackagePath,
			FAvidScriptCSharpBuildInvocationCounts::FromResult(InvocationResult),
			OutResult);
		return bBuildSucceeded;
	}

	RemoveAvidScriptCSharpFinalLoadableArtifacts(NormalizedConfig);
	const FString BootstrapRoot = MakeAvidScriptCSharpBootstrapRoot();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*BootstrapRoot, false, true);
	};

	FAvidScriptEditorCSharpBuildConfig BootstrapConfig = NormalizedConfig;
	BootstrapConfig.OutputRoot = BootstrapRoot;
	BootstrapConfig.ReportPath = MakeReportPathForOutputRoot(BootstrapRoot, BootstrapConfig.ArtifactStem);
	BootstrapConfig.ManifestPath = MakeManifestPathForOutputRoot(BootstrapRoot, BootstrapConfig.ArtifactStem);
	BootstrapConfig.RuntimeBindingPackagePath.Reset();
	BootstrapConfig.PreparedBuildReportPath.Reset();
	BootstrapConfig.bOmitRuntimeBindingPackage = false;

	FAvidScriptEditorCSharpBuildResult BootstrapResult;
	if (!FAvidScriptEditorCSharpBuildInvoker::BuildOnce(BootstrapConfig, BootstrapResult))
	{
		ApplyAvidScriptCSharpBuildInvocationOutcome(
			BootstrapResult,
			NormalizedConfig,
			AuthorizationBindingPackagePath,
			FString(),
			FAvidScriptCSharpBuildInvocationCounts::FromResult(BootstrapResult),
			OutResult);
		return false;
	}

	FAvidScriptFrontendReport BootstrapReport;
	FAvidScriptFrontendReportLoadResult BootstrapReportLoadResult;
	if (!FAvidScriptFrontendReportReader::LoadFromFile(
			BootstrapConfig.ReportPath,
			BootstrapReport,
			BootstrapReportLoadResult))
	{
		ApplyAvidScriptCSharpBuildInvocationOutcome(
			BootstrapResult,
			NormalizedConfig,
			AuthorizationBindingPackagePath,
			FString(),
			FAvidScriptCSharpBuildInvocationCounts::FromResult(BootstrapResult),
			OutResult);
		SetAvidScriptCSharpBuildFailure(
			BootstrapReportLoadResult.ErrorCategory.IsEmpty()
				? FString(TEXT("bootstrap_report_invalid"))
				: BootstrapReportLoadResult.ErrorCategory,
			BootstrapReportLoadResult.ErrorMessage,
			TEXT("repair bootstrap binding provenance and retry the custom C# build"),
			OutResult);
		return false;
	}
	if (!BootstrapReport.BindingPackage.bPresent
		|| BootstrapReport.BindingPackage.UsedImportCount
			!= BootstrapReport.BindingPackage.UsedImports.Num())
	{
		ApplyAvidScriptCSharpBuildInvocationOutcome(
			BootstrapResult,
			NormalizedConfig,
			AuthorizationBindingPackagePath,
			FString(),
			FAvidScriptCSharpBuildInvocationCounts::FromResult(BootstrapResult),
			OutResult);
		SetAvidScriptCSharpBuildFailure(
			TEXT("bootstrap_binding_provenance_invalid"),
			TEXT("Bootstrap report does not contain a complete binding_package.used_imports provenance set."),
			TEXT("rerun bootstrap with the current semantic and build report toolchain"),
			OutResult);
		return false;
	}

	FAvidScriptEditorCSharpBuildConfig FinalConfig = NormalizedConfig;
	FinalConfig.PreparedBuildReportPath = BootstrapConfig.ReportPath;
	if (BootstrapReport.BindingPackage.UsedImports.IsEmpty())
	{
		FinalConfig.RuntimeBindingPackagePath.Reset();
		FinalConfig.bOmitRuntimeBindingPackage = true;
		RuntimeBindingPackagePath.Reset();
	}
	else
	{
		FAvidScriptCSharpBindingEmitResult RuntimePackage;
		FAvidScriptEditorCSharpBindingSliceResult SliceResult;
		if (!FAvidScriptEditorCSharpBindingSliceService::Publish(
				BootstrapReport.BindingPackage.DescriptorFile,
				BootstrapReport.BindingPackage,
				FAvidScriptEditorCSharpBindingEmitter::GetDefaultOutputRoot(),
				RuntimePackage,
				SliceResult))
		{
			ApplyAvidScriptCSharpBuildInvocationOutcome(
				BootstrapResult,
				NormalizedConfig,
				AuthorizationBindingPackagePath,
				FString(),
				FAvidScriptCSharpBuildInvocationCounts::FromResult(BootstrapResult),
				OutResult);
			SetAvidScriptCSharpBuildFailure(
				SliceResult.ErrorCategory.IsEmpty()
					? FString(TEXT("binding_slice_failed"))
					: SliceResult.ErrorCategory,
				SliceResult.ErrorMessage.IsEmpty()
					? FString(TEXT("Runtime binding package slice could not be published."))
					: SliceResult.ErrorMessage,
				SliceResult.NextAction,
				OutResult);
			return false;
		}
		RuntimeBindingPackagePath = NormalizeAvidScriptCSharpBuildPathCopy(RuntimePackage.ManifestPath);
		FinalConfig.RuntimeBindingPackagePath = RuntimeBindingPackagePath;
		FinalConfig.bOmitRuntimeBindingPackage = false;
	}

	FAvidScriptEditorCSharpBuildResult FinalResult;
	const bool bFinalBuildSucceeded = FAvidScriptEditorCSharpBuildInvoker::BuildOnce(FinalConfig, FinalResult);
	FAvidScriptEditorCSharpBuildResult AggregateResult = FinalResult;
	if (FinalResult.SemanticCacheLookup == TEXT("disabled")
		&& BootstrapResult.SemanticCacheLookup != TEXT("disabled")
		&& !BootstrapResult.SemanticCacheLookup.IsEmpty())
	{
		CopyAvidScriptCSharpSemanticCacheAudit(BootstrapResult, AggregateResult);
	}
	const FAvidScriptCSharpBuildInvocationCounts TotalInvocationCounts = {
		BootstrapResult.BuildInvocationCount + FinalResult.BuildInvocationCount,
		BootstrapResult.FrontendInvocationCount + FinalResult.FrontendInvocationCount,
		BootstrapResult.SemanticInvocationCount + FinalResult.SemanticInvocationCount,
		BootstrapResult.GuestIrInvocationCount + FinalResult.GuestIrInvocationCount,
		BootstrapResult.WasmBackendInvocationCount + FinalResult.WasmBackendInvocationCount
	};
	ApplyAvidScriptCSharpBuildInvocationOutcome(
		AggregateResult,
		NormalizedConfig,
		AuthorizationBindingPackagePath,
		RuntimeBindingPackagePath,
		TotalInvocationCounts,
		OutResult);
	return bFinalBuildSucceeded;
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
