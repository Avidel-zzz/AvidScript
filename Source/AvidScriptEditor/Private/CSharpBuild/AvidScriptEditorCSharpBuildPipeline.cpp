#include "CSharpBuild/AvidScriptEditorCSharpBuildPipeline.h"

#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptFrontendReport.h"
#include "CSharpBuild/AvidScriptEditorCSharpBindingSliceService.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(
	LogAvidScriptCSharpBuildPipeline,
	Log,
	All);

namespace
{
void NormalizeAvidScriptCSharpBuildPipelinePath(FString& Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}
}

FString NormalizeAvidScriptCSharpBuildPipelinePathCopy(FString Path)
{
	NormalizeAvidScriptCSharpBuildPipelinePath(Path);
	return Path;
}

void SetAvidScriptCSharpBuildPipelineFailure(
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

void SetAvidScriptCSharpBuildPipelineResultMetadata(
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

void ApplyAvidScriptCSharpBuildPipelineOutcome(
	const FAvidScriptEditorCSharpBuildResult& InvocationResult,
	const FAvidScriptEditorCSharpBuildPlan& Plan,
	const FAvidScriptCSharpBuildInvocationCounts& InvocationCounts,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutResult = InvocationResult;
	SetAvidScriptCSharpBuildPipelineResultMetadata(
		Plan.FinalConfig,
		Plan.AuthorizationBindingPackagePath,
		Plan.RuntimeBindingPackagePath,
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

TArray<FString> GetAvidScriptCSharpCommittedArtifactPaths(
	const FAvidScriptEditorCSharpBuildConfig& Config)
{
	return {
		Config.ReportPath,
		Config.ManifestPath,
		FPaths::Combine(Config.OutputRoot, Config.ArtifactStem + TEXT(".wasm"))
	};
}

bool BeginAvidScriptCSharpArtifactTransaction(
	FAvidScriptEditorCSharpBuildPlan& Plan,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	Plan.ArtifactBackupRoot =
		NormalizeAvidScriptCSharpBuildPipelinePathCopy(FPaths::Combine(
			FPaths::ProjectIntermediateDir(),
			TEXT("AvidScript"),
			TEXT("CSharpBuildTransactions"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!IFileManager::Get().MakeDirectory(
			*Plan.ArtifactBackupRoot,
			true))
	{
		SetAvidScriptCSharpBuildPipelineFailure(
			TEXT("artifact_backup_directory_failed"),
			FString::Printf(
				TEXT("C# committed artifact backup directory could not be created: %s"),
				*Plan.ArtifactBackupRoot),
			TEXT("verify the project Intermediate directory is writable and retry"),
			OutResult);
		Plan.ArtifactBackupRoot.Reset();
		return false;
	}

	const TArray<FString> CommittedPaths =
		GetAvidScriptCSharpCommittedArtifactPaths(Plan.FinalConfig);
	for (int32 Index = 0; Index < CommittedPaths.Num(); ++Index)
	{
		FAvidScriptEditorCSharpBuildArtifactBackup Backup;
		Backup.CommittedPath = CommittedPaths[Index];
		Backup.bExisted =
			!Backup.CommittedPath.IsEmpty()
			&& FPaths::FileExists(Backup.CommittedPath);
		if (Backup.bExisted)
		{
			Backup.BackupPath = FPaths::Combine(
				Plan.ArtifactBackupRoot,
				FString::Printf(
					TEXT("%d_%s"),
					Index,
					*FPaths::GetCleanFilename(Backup.CommittedPath)));
			if (IFileManager::Get().Copy(
					*Backup.BackupPath,
					*Backup.CommittedPath,
					true,
					true)
				!= COPY_OK)
			{
				SetAvidScriptCSharpBuildPipelineFailure(
					TEXT("artifact_backup_failed"),
					FString::Printf(
						TEXT("C# committed artifact could not be backed up: %s"),
						*Backup.CommittedPath),
					TEXT("verify the artifact and Intermediate directories are readable and writable"),
					OutResult);
				IFileManager::Get().DeleteDirectory(
					*Plan.ArtifactBackupRoot,
					false,
					true);
				Plan.ArtifactBackupRoot.Reset();
				Plan.ArtifactBackups.Reset();
				return false;
			}
		}
		Plan.ArtifactBackups.Add(MoveTemp(Backup));
	}
	Plan.bArtifactTransactionActive = true;
	return true;
}

bool FinishAvidScriptCSharpArtifactTransaction(
	FAvidScriptEditorCSharpBuildPlan& Plan,
	const bool bCommit,
	FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	if (!Plan.bArtifactTransactionActive)
	{
		return true;
	}

	if (!bCommit)
	{
		for (const FAvidScriptEditorCSharpBuildArtifactBackup& Backup :
			Plan.ArtifactBackups)
		{
			if (Backup.CommittedPath.IsEmpty())
			{
				continue;
			}
			if (Backup.bExisted)
			{
				if (IFileManager::Get().Copy(
						*Backup.CommittedPath,
						*Backup.BackupPath,
						true,
						true)
					!= COPY_OK)
				{
					OutErrorMessage = FString::Printf(
						TEXT("C# committed artifact could not be restored: %s"),
						*Backup.CommittedPath);
					return false;
				}
			}
			else if (FPaths::FileExists(Backup.CommittedPath)
				&& !IFileManager::Get().Delete(
					*Backup.CommittedPath,
					false,
					true,
					true))
			{
				OutErrorMessage = FString::Printf(
					TEXT("New C# artifact could not be removed during rollback: %s"),
					*Backup.CommittedPath);
				return false;
			}
		}
	}

	IFileManager::Get().DeleteDirectory(
		*Plan.ArtifactBackupRoot,
		false,
		true);
	Plan.ArtifactBackupRoot.Reset();
	Plan.ArtifactBackups.Reset();
	Plan.bArtifactTransactionActive = false;
	return true;
}

FString MakeAvidScriptCSharpBootstrapRoot()
{
	return NormalizeAvidScriptCSharpBuildPipelinePathCopy(FPaths::Combine(
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

	SetAvidScriptCSharpBuildPipelineFailure(
		ErrorCategory,
		FString::Printf(TEXT("%s does not exist: %s"), *Description, *PackagePath),
		TEXT("publish a generated binding package or update the profile package path"),
		OutResult);
	return false;
}
} // namespace

bool FAvidScriptEditorCSharpBuildPipeline::Prepare(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildPlan& OutPlan,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutPlan = FAvidScriptEditorCSharpBuildPlan();
	OutResult = FAvidScriptEditorCSharpBuildResult();

#if PLATFORM_WINDOWS
	FAvidScriptEditorCSharpBuildConfig NormalizedConfig = Config;
	if (NormalizedConfig.BuildScriptPath.IsEmpty())
	{
		NormalizedConfig.BuildScriptPath =
			FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	}
	if (NormalizedConfig.SourcePath.IsEmpty())
	{
		NormalizedConfig.SourcePath =
			FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath();
	}
	if (NormalizedConfig.ProjectPath.IsEmpty())
	{
		NormalizedConfig.ProjectPath =
			FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	}
	if (NormalizedConfig.OutputRoot.IsEmpty())
	{
		NormalizedConfig.OutputRoot =
			FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleOutputRoot();
	}
	if (NormalizedConfig.ModuleId.IsEmpty())
	{
		NormalizedConfig.ModuleId =
			FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleModuleId();
	}
	if (NormalizedConfig.ArtifactStem.IsEmpty())
	{
		NormalizedConfig.ArtifactStem =
			FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleArtifactStem();
	}
	if (NormalizedConfig.ReportPath.IsEmpty())
	{
		NormalizedConfig.ReportPath =
			FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
				NormalizedConfig.OutputRoot,
				NormalizedConfig.ArtifactStem);
	}
	if (NormalizedConfig.ManifestPath.IsEmpty())
	{
		NormalizedConfig.ManifestPath =
			FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
				NormalizedConfig.OutputRoot,
				NormalizedConfig.ArtifactStem);
	}
	if (NormalizedConfig.Configuration.IsEmpty())
	{
		NormalizedConfig.Configuration = TEXT("Release");
	}

	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.BuildScriptPath);
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.SourcePath);
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.ProjectPath);
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.OutputRoot);
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.ReportPath);
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.ManifestPath);
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.SemanticCacheRoot);
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.BindingPackagePath);
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.RuntimeBindingPackagePath);
	NormalizedConfig.PreparedBuildReportPath.Reset();
	NormalizeAvidScriptCSharpBuildPipelinePath(NormalizedConfig.DotNetPath);

	OutPlan.FinalConfig = NormalizedConfig;
	OutPlan.AuthorizationBindingPackagePath = NormalizedConfig.BindingPackagePath;
	OutPlan.RuntimeBindingPackagePath = NormalizedConfig.bOmitRuntimeBindingPackage
		? FString()
		: NormalizedConfig.RuntimeBindingPackagePath;
	SetAvidScriptCSharpBuildPipelineResultMetadata(
		NormalizedConfig,
		OutPlan.AuthorizationBindingPackagePath,
		OutPlan.RuntimeBindingPackagePath,
		FAvidScriptCSharpBuildInvocationCounts(),
		OutResult);

	if (NormalizedConfig.BuildScriptPath.IsEmpty()
		|| !FPaths::FileExists(NormalizedConfig.BuildScriptPath))
	{
		SetAvidScriptCSharpBuildPipelineFailure(
			TEXT("build_script_missing"),
			FString::Printf(
				TEXT("C# build script does not exist: %s"),
				*NormalizedConfig.BuildScriptPath),
			TEXT("verify BuildCSharpActorLifecycle.ps1 exists in the plugin Build directory"),
			OutResult);
		return false;
	}
	if (NormalizedConfig.SourcePath.IsEmpty()
		|| !FPaths::FileExists(NormalizedConfig.SourcePath))
	{
		SetAvidScriptCSharpBuildPipelineFailure(
			TEXT("source_missing"),
			FString::Printf(
				TEXT("C# source file does not exist: %s"),
				*NormalizedConfig.SourcePath),
			TEXT("choose an existing C# source file or regenerate the default C# profile"),
			OutResult);
		return false;
	}
	if (!NormalizedConfig.ProjectPath.IsEmpty()
		&& !FPaths::FileExists(NormalizedConfig.ProjectPath))
	{
		SetAvidScriptCSharpBuildPipelineFailure(
			TEXT("project_missing"),
			FString::Printf(
				TEXT("C# project file does not exist: %s"),
				*NormalizedConfig.ProjectPath),
			TEXT("choose an existing C# project file or update project_path in the C# profile"),
			OutResult);
		return false;
	}

	const bool bRequiresGeneratedBindingPackage =
		!NormalizedConfig.SourcePath.Equals(
			FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath(),
			ESearchCase::IgnoreCase);
	OutPlan.bAutomaticBindingSlice =
		bRequiresGeneratedBindingPackage
		&& OutPlan.AuthorizationBindingPackagePath.IsEmpty();
	if (OutPlan.bAutomaticBindingSlice
		&& (!NormalizedConfig.RuntimeBindingPackagePath.IsEmpty()
			|| NormalizedConfig.bOmitRuntimeBindingPackage))
	{
		SetAvidScriptCSharpBuildPipelineFailure(
			TEXT("binding_package_strategy_invalid"),
			TEXT("Runtime package overrides require an explicit authorization BindingPackagePath."),
			TEXT("set BindingPackagePath or clear runtime package overrides to use automatic slicing"),
			OutResult);
		return false;
	}

	if (OutPlan.bAutomaticBindingSlice)
	{
		FAvidScriptCSharpBindingEmitResult BindingEmitResult;
		if (!FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(
				BindingEmitResult))
		{
			SetAvidScriptCSharpBuildPipelineFailure(
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
		OutPlan.AuthorizationBindingPackagePath =
			NormalizeAvidScriptCSharpBuildPipelinePathCopy(
				BindingEmitResult.ManifestPath);
		OutPlan.FinalConfig.BindingPackagePath =
			OutPlan.AuthorizationBindingPackagePath;
		SetAvidScriptCSharpBuildPipelineResultMetadata(
			OutPlan.FinalConfig,
			OutPlan.AuthorizationBindingPackagePath,
			FString(),
			FAvidScriptCSharpBuildInvocationCounts(),
			OutResult);
	}

	if (!ValidateAvidScriptCSharpBuildPackagePath(
			OutPlan.AuthorizationBindingPackagePath,
			TEXT("binding_package_missing"),
			TEXT("C# authorization binding package manifest"),
			OutResult)
		|| !ValidateAvidScriptCSharpBuildPackagePath(
			OutPlan.FinalConfig.RuntimeBindingPackagePath,
			TEXT("runtime_binding_package_missing"),
			TEXT("C# runtime binding package manifest"),
			OutResult))
	{
		return false;
	}
	if (!BeginAvidScriptCSharpArtifactTransaction(OutPlan, OutResult))
	{
		return false;
	}

	if (!OutPlan.bAutomaticBindingSlice)
	{
		if (!OutPlan.FinalConfig.bOmitRuntimeBindingPackage
			&& OutPlan.RuntimeBindingPackagePath.IsEmpty())
		{
			OutPlan.RuntimeBindingPackagePath =
				OutPlan.AuthorizationBindingPackagePath;
		}
		return true;
	}

	OutPlan.BootstrapRoot = MakeAvidScriptCSharpBootstrapRoot();
	OutPlan.BootstrapConfig = OutPlan.FinalConfig;
	OutPlan.BootstrapConfig.OutputRoot = OutPlan.BootstrapRoot;
	OutPlan.BootstrapConfig.ReportPath =
		FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
			OutPlan.BootstrapRoot,
			OutPlan.BootstrapConfig.ArtifactStem);
	OutPlan.BootstrapConfig.ManifestPath =
		FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
			OutPlan.BootstrapRoot,
			OutPlan.BootstrapConfig.ArtifactStem);
	OutPlan.BootstrapConfig.RuntimeBindingPackagePath.Reset();
	OutPlan.BootstrapConfig.PreparedBuildReportPath.Reset();
	OutPlan.BootstrapConfig.bOmitRuntimeBindingPackage = false;
	return true;
#else
	SetAvidScriptCSharpBuildPipelineFailure(
		TEXT("platform_unsupported"),
		TEXT("C# build invocation is currently implemented only for Windows Editor hosts."),
		TEXT("use a Windows Editor host for C# build-and-bind until other platforms are implemented"),
		OutResult);
	return false;
#endif
}

bool FAvidScriptEditorCSharpBuildPipeline::CompleteBootstrap(
	FAvidScriptEditorCSharpBuildPlan& Plan,
	const FAvidScriptEditorCSharpBuildResult& BootstrapResult,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	Plan.BootstrapResult = BootstrapResult;
	Plan.bBootstrapCompleted = true;
	if (!BootstrapResult.bSucceeded)
	{
		ApplyAvidScriptCSharpBuildPipelineOutcome(
			BootstrapResult,
			Plan,
			FAvidScriptCSharpBuildInvocationCounts::FromResult(BootstrapResult),
			OutResult);
		return false;
	}

	FAvidScriptFrontendReport BootstrapReport;
	FAvidScriptFrontendReportLoadResult BootstrapReportLoadResult;
	if (!FAvidScriptFrontendReportReader::LoadFromFile(
			Plan.BootstrapConfig.ReportPath,
			BootstrapReport,
			BootstrapReportLoadResult))
	{
		ApplyAvidScriptCSharpBuildPipelineOutcome(
			BootstrapResult,
			Plan,
			FAvidScriptCSharpBuildInvocationCounts::FromResult(BootstrapResult),
			OutResult);
		SetAvidScriptCSharpBuildPipelineFailure(
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
		ApplyAvidScriptCSharpBuildPipelineOutcome(
			BootstrapResult,
			Plan,
			FAvidScriptCSharpBuildInvocationCounts::FromResult(BootstrapResult),
			OutResult);
		SetAvidScriptCSharpBuildPipelineFailure(
			TEXT("bootstrap_binding_provenance_invalid"),
			TEXT("Bootstrap report does not contain a complete binding_package.used_imports provenance set."),
			TEXT("rerun bootstrap with the current semantic and build report toolchain"),
			OutResult);
		return false;
	}

	Plan.FinalConfig.PreparedBuildReportPath =
		Plan.BootstrapConfig.ReportPath;
	if (BootstrapReport.BindingPackage.UsedImports.IsEmpty())
	{
		Plan.FinalConfig.RuntimeBindingPackagePath.Reset();
		Plan.FinalConfig.bOmitRuntimeBindingPackage = true;
		Plan.RuntimeBindingPackagePath.Reset();
		return true;
	}

	FAvidScriptCSharpBindingEmitResult RuntimePackage;
	FAvidScriptEditorCSharpBindingSliceResult SliceResult;
	if (!FAvidScriptEditorCSharpBindingSliceService::Publish(
			BootstrapReport.BindingPackage.DescriptorFile,
			BootstrapReport.BindingPackage,
			FAvidScriptEditorCSharpBindingEmitter::GetDefaultOutputRoot(),
			RuntimePackage,
			SliceResult))
	{
		ApplyAvidScriptCSharpBuildPipelineOutcome(
			BootstrapResult,
			Plan,
			FAvidScriptCSharpBuildInvocationCounts::FromResult(BootstrapResult),
			OutResult);
		SetAvidScriptCSharpBuildPipelineFailure(
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

	Plan.RuntimeBindingPackagePath =
		NormalizeAvidScriptCSharpBuildPipelinePathCopy(
			RuntimePackage.ManifestPath);
	Plan.FinalConfig.RuntimeBindingPackagePath =
		Plan.RuntimeBindingPackagePath;
	Plan.FinalConfig.bOmitRuntimeBindingPackage = false;
	return true;
}

bool FAvidScriptEditorCSharpBuildPipeline::CompleteFinal(
	FAvidScriptEditorCSharpBuildPlan& Plan,
	const FAvidScriptEditorCSharpBuildResult& FinalResult,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	auto FinishArtifactTransaction = [&Plan, &OutResult](
		const bool bCommit)
	{
		FString RollbackError;
		if (FinishAvidScriptCSharpArtifactTransaction(
				Plan,
				bCommit,
				RollbackError))
		{
			return bCommit;
		}

		const FString BuildError = OutResult.ErrorMessage;
		SetAvidScriptCSharpBuildPipelineFailure(
			TEXT("artifact_rollback_failed"),
			RollbackError,
			FString::Printf(
				TEXT("recover the previous artifacts from %s before rebuilding; original failure: %s"),
				*Plan.ArtifactBackupRoot,
				BuildError.IsEmpty() ? TEXT("<none>") : *BuildError),
			OutResult);
		return false;
	};

	if (!Plan.bAutomaticBindingSlice)
	{
		ApplyAvidScriptCSharpBuildPipelineOutcome(
			FinalResult,
			Plan,
			FAvidScriptCSharpBuildInvocationCounts::FromResult(FinalResult),
			OutResult);
		return FinishArtifactTransaction(FinalResult.bSucceeded);
	}

	FAvidScriptEditorCSharpBuildResult AggregateResult = FinalResult;
	if (FinalResult.SemanticCacheLookup == TEXT("disabled")
		&& Plan.BootstrapResult.SemanticCacheLookup != TEXT("disabled")
		&& !Plan.BootstrapResult.SemanticCacheLookup.IsEmpty())
	{
		CopyAvidScriptCSharpSemanticCacheAudit(
			Plan.BootstrapResult,
			AggregateResult);
	}
	const FAvidScriptCSharpBuildInvocationCounts TotalInvocationCounts = {
		Plan.BootstrapResult.BuildInvocationCount
			+ FinalResult.BuildInvocationCount,
		Plan.BootstrapResult.FrontendInvocationCount
			+ FinalResult.FrontendInvocationCount,
		Plan.BootstrapResult.SemanticInvocationCount
			+ FinalResult.SemanticInvocationCount,
		Plan.BootstrapResult.GuestIrInvocationCount
			+ FinalResult.GuestIrInvocationCount,
		Plan.BootstrapResult.WasmBackendInvocationCount
			+ FinalResult.WasmBackendInvocationCount
	};
	ApplyAvidScriptCSharpBuildPipelineOutcome(
		AggregateResult,
		Plan,
		TotalInvocationCounts,
		OutResult);
	return FinishArtifactTransaction(FinalResult.bSucceeded);
}

void FAvidScriptEditorCSharpBuildPipeline::Cleanup(
	FAvidScriptEditorCSharpBuildPlan& Plan)
{
	FString RollbackError;
	if (!FinishAvidScriptCSharpArtifactTransaction(
			Plan,
			false,
			RollbackError))
	{
		UE_LOG(
			LogAvidScriptCSharpBuildPipeline,
			Error,
			TEXT("C# build artifact rollback failed during cleanup. backup=%s error=%s"),
			*Plan.ArtifactBackupRoot,
			*RollbackError);
	}
	if (!Plan.BootstrapRoot.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(
			*Plan.BootstrapRoot,
			false,
			true);
		Plan.BootstrapRoot.Reset();
	}
}
