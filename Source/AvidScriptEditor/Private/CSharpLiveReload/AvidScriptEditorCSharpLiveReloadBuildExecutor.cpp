#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadBuildExecutor.h"

#include "AvidScriptEditorCSharpBuildService.h"

#include "GameFramework/Actor.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

namespace
{
FString NormalizeAvidScriptLiveReloadBuildExecutorPath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}
	return Path;
}

void SetAvidScriptLiveReloadBuildExecutorFailure(
	const EAvidScriptEditorCSharpLiveReloadBuildStatus Status,
	const FString& ErrorCategory,
	const FString& CauseErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.Status = Status;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.CauseErrorCategory = CauseErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}
} // namespace

FAvidScriptEditorCSharpLiveReloadBuildExecutor::FAvidScriptEditorCSharpLiveReloadBuildExecutor()
	: FAvidScriptEditorCSharpLiveReloadBuildExecutor(
		[](const FString& ProfilePath, FAvidScriptEditorCSharpProfileLoadResult& OutResult)
		{
			return FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, OutResult);
		},
		[](const FAvidScriptEditorCSharpBuildRequest& Request, FAvidScriptEditorCSharpBuildResult& OutResult)
		{
			return FAvidScriptEditorCSharpBuildService::BuildProfile(Request, OutResult);
		},
		[](
			const FString& ReportPath,
			AActor* TargetActor,
			FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(
				ReportPath,
				TargetActor,
				OutResult);
		})
{
}

FAvidScriptEditorCSharpLiveReloadBuildExecutor::FAvidScriptEditorCSharpLiveReloadBuildExecutor(
	FLoadProfile InLoadProfile,
	FBuildProfile InBuildProfile,
	FApplyReport InApplyReport)
	: LoadProfile(MoveTemp(InLoadProfile))
	, BuildProfile(MoveTemp(InBuildProfile))
	, ApplyReport(MoveTemp(InApplyReport))
{
}

bool FAvidScriptEditorCSharpLiveReloadBuildExecutor::Execute(
	const FString& ProfilePath,
	AActor* TargetActor,
	FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult) const
{
	OutResult = FAvidScriptEditorCSharpLiveReloadBuildResult();
	OutResult.ProfilePath = NormalizeAvidScriptLiveReloadBuildExecutorPath(ProfilePath);
	if (!IsValid(TargetActor))
	{
		SetAvidScriptLiveReloadBuildExecutorFailure(
			EAvidScriptEditorCSharpLiveReloadBuildStatus::TargetUnavailable,
			TEXT("live_reload_target_unavailable"),
			TEXT("actor_invalid"),
			TEXT("The fixed C# live reload target Actor is no longer valid."),
			TEXT("select a valid Actor and restart Project C# Auto Live Reload"),
			OutResult);
		return false;
	}
	OutResult.TargetActorPath = TargetActor->GetPathName();

	if (!LoadProfile || !BuildProfile || !ApplyReport)
	{
		SetAvidScriptLiveReloadBuildExecutorFailure(
			EAvidScriptEditorCSharpLiveReloadBuildStatus::ProfileFailed,
			TEXT("live_reload_executor_invalid"),
			TEXT("executor_dependency_missing"),
			TEXT("C# live reload build executor dependencies are incomplete."),
			TEXT("restart the editor or reload the AvidScriptEditor module"),
			OutResult);
		return false;
	}

	const bool bProfileLoaded = LoadProfile(ProfilePath, OutResult.ProfileResult);
	if (!OutResult.ProfileResult.NormalizedProfilePath.IsEmpty())
	{
		OutResult.ProfilePath = OutResult.ProfileResult.NormalizedProfilePath;
	}
	if (!bProfileLoaded || !OutResult.ProfileResult.bSucceeded)
	{
		SetAvidScriptLiveReloadBuildExecutorFailure(
			EAvidScriptEditorCSharpLiveReloadBuildStatus::ProfileFailed,
			TEXT("live_reload_profile_failed"),
			OutResult.ProfileResult.ErrorCategory,
			OutResult.ProfileResult.ErrorMessage.IsEmpty()
				? FString(TEXT("C# live reload profile could not be loaded."))
				: OutResult.ProfileResult.ErrorMessage,
			OutResult.ProfileResult.NextAction.IsEmpty()
				? FString(TEXT("repair the project C# profile and save the script again"))
				: OutResult.ProfileResult.NextAction,
			OutResult);
		return false;
	}

	const bool bBuildSucceeded = BuildProfile(
		FAvidScriptEditorCSharpProfileService::MakeBuildRequest(
			OutResult.ProfileResult),
		OutResult.BuildResult);
	if (!bBuildSucceeded || !OutResult.BuildResult.bSucceeded)
	{
		SetAvidScriptLiveReloadBuildExecutorFailure(
			EAvidScriptEditorCSharpLiveReloadBuildStatus::BuildFailed,
			TEXT("live_reload_build_failed"),
			OutResult.BuildResult.ErrorCategory,
			OutResult.BuildResult.ErrorMessage.IsEmpty()
				? FString(TEXT("C# live reload build failed."))
				: OutResult.BuildResult.ErrorMessage,
			OutResult.BuildResult.NextAction.IsEmpty()
				? FString(TEXT("fix the C# build diagnostic and save the script again"))
				: OutResult.BuildResult.NextAction,
			OutResult);
		return false;
	}

	const bool bBindingSucceeded = ApplyReport(
		OutResult.BuildResult.ReportPath,
		TargetActor,
		OutResult.BindingResult);
	if (!bBindingSucceeded || !OutResult.BindingResult.bSucceeded)
	{
		SetAvidScriptLiveReloadBuildExecutorFailure(
			EAvidScriptEditorCSharpLiveReloadBuildStatus::BindingFailed,
			TEXT("live_reload_binding_failed"),
			OutResult.BindingResult.ErrorCategory,
			OutResult.BindingResult.ErrorMessage.IsEmpty()
				? FString(TEXT("C# live reload could not apply the build report to the fixed Actor."))
				: OutResult.BindingResult.ErrorMessage,
			OutResult.BindingResult.NextAction.IsEmpty()
				? FString(TEXT("repair the candidate manifest or restart live reload for the target Actor"))
				: OutResult.BindingResult.NextAction,
			OutResult);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.Status = EAvidScriptEditorCSharpLiveReloadBuildStatus::Succeeded;
	return true;
}
