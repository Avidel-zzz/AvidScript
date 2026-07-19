#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.h"

#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadCompletion.h"

#include "GameFramework/Actor.h"

namespace
{
bool IsAvidScriptLiveReloadBuildTargetValid(AActor* Actor)
{
	return IsValid(Actor) && !Actor->IsActorBeingDestroyed();
}
} // namespace

bool FAvidScriptEditorCSharpLiveReloadService::Tick()
{
	if (!IsRunning())
	{
		return false;
	}

	const double CurrentSeconds = NowSeconds();
	DrainPendingChanges(CurrentSeconds);
	AActor* FixedTarget = TargetActor.Get();
	if (!IsAvidScriptLiveReloadBuildTargetValid(FixedTarget))
	{
		const bool bDestroyedDuringBuild = ActiveBuildJob.IsValid();
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::TargetUnavailable,
			TEXT("live_reload_target_unavailable"),
			bDestroyedDuringBuild
				? FString(TEXT("actor_destroyed_during_build"))
				: FString(TEXT("actor_destroyed")),
			bDestroyedDuringBuild
				? FString(TEXT("The fixed Project C# Auto Live Reload Actor was destroyed during an asynchronous build."))
				: FString(TEXT("The fixed Project C# Auto Live Reload Actor was destroyed.")),
			TEXT("select another Actor and restart Project C# Auto Live Reload"));
		LastResult.TargetActorPath = FixedTargetPath;
		StopInternal(true);
		return false;
	}

	if (ActiveBuildJob)
	{
		IAvidScriptEditorCSharpAsyncBuildJob* TickingJob =
			ActiveBuildJob.Get();
		const uint64 TickingJobSerial = ActiveBuildJobSerial;
		TickingJob->Tick();
		if (!IsRunning()
			|| ActiveBuildJob.Get() != TickingJob
			|| ActiveBuildJobSerial != TickingJobSerial)
		{
			return IsRunning();
		}
		LastResult.AsyncProgress = ActiveBuildJob->GetProgress();
		LastResult.Stats = Coordinator.GetStats();
		if (ActiveBuildJob->IsFinished()
			&& !CompleteActiveBuildJob(
				FixedTarget,
				TickingJob,
				TickingJobSerial))
		{
			return IsRunning();
		}
		if (!IsRunning())
		{
			return false;
		}
	}

	if (!ActiveBuildJob)
	{
		TryStartBuildJob(NowSeconds());
	}
	return IsRunning();
}

bool FAvidScriptEditorCSharpLiveReloadService::TryStartBuildJob(
	const double CurrentSeconds)
{
	const TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest> Request =
		Coordinator.TryBeginBuild(CurrentSeconds);
	if (!Request.IsSet())
	{
		return false;
	}

	TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob> NewJob =
		CreateBuildJob();
	if (!NewJob)
	{
		Coordinator.CompleteBuild(*Request, false);
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed,
			TEXT("live_reload_build_job_unavailable"),
			TEXT("job_factory_returned_null"),
			TEXT("Project C# Auto Live Reload could not create an asynchronous build job."),
			TEXT("restart the editor or reload the AvidScriptEditor module"));
		LastResult.bRunning = true;
		LastResult.Request = *Request;
		LastResult.Stats = Coordinator.GetStats();
		return false;
	}

	ActiveBuildRequest = *Request;
	ActiveBuildJobSerial = NextBuildJobSerial++;
	ActiveBuildJob = MoveTemp(NewJob);
	const bool bStarted = ActiveBuildJob->Start(ActiveConfig.ProfilePath);
	if (!bStarted && !ActiveBuildJob->IsFinished())
	{
		ActiveBuildJob->Cancel();
	}

	LastResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	LastResult.bSucceeded = false;
	LastResult.bRunning = true;
	LastResult.Status =
		EAvidScriptEditorCSharpLiveReloadServiceStatus::Building;
	LastResult.WorkspaceRoot = ActiveConfig.WorkspaceRoot;
	LastResult.ProfilePath = ActiveConfig.ProfilePath;
	LastResult.TargetActorPath = FixedTargetPath;
	LastResult.Request = ActiveBuildRequest;
	LastResult.Stats = Coordinator.GetStats();
	LastResult.AsyncProgress = ActiveBuildJob->GetProgress();
	return true;
}

bool FAvidScriptEditorCSharpLiveReloadService::CompleteActiveBuildJob(
	AActor* FixedTarget,
	IAvidScriptEditorCSharpAsyncBuildJob* ExpectedJob,
	const uint64 ExpectedJobSerial)
{
	if (ExpectedJob == nullptr
		|| ActiveBuildJob.Get() != ExpectedJob
		|| ActiveBuildJobSerial != ExpectedJobSerial)
	{
		return false;
	}

	const FAvidScriptEditorCSharpLiveReloadBuildRequest CompletingRequest =
		ActiveBuildRequest;
	const FAvidScriptEditorCSharpAsyncBuildProgress CompletionProgress =
		ActiveBuildJob->GetProgress();
	FAvidScriptEditorCSharpAsyncBuildResult AsyncResult;
	if (!ActiveBuildJob->ConsumeResult(AsyncResult))
	{
		AsyncResult.ErrorCategory =
			TEXT("live_reload_build_result_unavailable");
		AsyncResult.ErrorMessage =
			TEXT("The asynchronous build job completed without a consumable result.");
		AsyncResult.NextAction =
			TEXT("save the C# source again or restart auto live reload");
	}

	const bool bRequestCurrent = IsActiveRequestCurrent();
	const bool bTargetCurrent =
		TargetActor.Get() == FixedTarget
		&& IsAvidScriptLiveReloadBuildTargetValid(FixedTarget)
		&& FixedTarget->GetPathName() == FixedTargetPath;
	if (!bRequestCurrent)
	{
		const bool bSameSession =
			Coordinator.GetStats().SessionGeneration ==
				CompletingRequest.SessionGeneration;
		ResetActiveBuildJob();
		if (bSameSession)
		{
			SetFailure(
				EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed,
				TEXT("live_reload_completion_rejected"),
				TEXT("request_generation_mismatch"),
				TEXT("The asynchronous build completion no longer matches the active request."),
				TEXT("restart Project C# Auto Live Reload"));
			LastResult.Request = CompletingRequest;
			LastResult.AsyncProgress = CompletionProgress;
			StopInternal(true);
		}
		return false;
	}
	if (!bTargetCurrent)
	{
		ResetActiveBuildJob();
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::TargetUnavailable,
			TEXT("live_reload_target_unavailable"),
			TEXT("actor_identity_changed_during_build"),
			TEXT("The fixed Project C# Auto Live Reload Actor identity changed during the asynchronous build."),
			TEXT("restart Project C# Auto Live Reload for the Actor"));
		LastResult.TargetActorPath = FixedTargetPath;
		LastResult.Request = CompletingRequest;
		LastResult.AsyncProgress = CompletionProgress;
		StopInternal(true);
		return false;
	}
	ResetActiveBuildJob();

	const bool bCanceled =
		CompletionProgress.Stage ==
			EAvidScriptEditorCSharpAsyncBuildStage::Canceled;
	const bool bReadyToBind =
		CompletionProgress.Stage ==
			EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind;
	if (!bReadyToBind && AsyncResult.bSucceeded)
	{
		AsyncResult.bSucceeded = false;
		if (bCanceled)
		{
			AsyncResult.ErrorCategory = TEXT("live_reload_build_canceled");
			if (AsyncResult.ErrorMessage.IsEmpty())
			{
				AsyncResult.ErrorMessage =
					TEXT("The asynchronous build was canceled before binding.");
			}
		}
		else
		{
			AsyncResult.ErrorCategory =
				TEXT("live_reload_build_terminal_state_invalid");
			AsyncResult.ErrorMessage =
				TEXT("The asynchronous build returned a successful payload from a non-bindable terminal stage.");
		}
	}
	FAvidScriptEditorCSharpLiveReloadBuildResult BuildResult;
	FAvidScriptEditorCSharpLiveReloadCompletion::FromAsyncBuild(
		AsyncResult,
		FixedTargetPath,
		BuildResult);
	if (bReadyToBind
		&& AsyncResult.bSucceeded
		&& AsyncResult.BuildResult.bSucceeded)
	{
		FAvidScriptEditorComponentBindingResult BindingResult;
		const bool bApplySucceeded = ApplyReport(
			AsyncResult.BuildResult.ReportPath,
			FixedTarget,
			BindingResult);
		FAvidScriptEditorCSharpLiveReloadCompletion::FromBinding(
			bApplySucceeded,
			MoveTemp(BindingResult),
			BuildResult);
		if (!IsAvidScriptLiveReloadBuildTargetValid(FixedTarget))
		{
			SetFailure(
				EAvidScriptEditorCSharpLiveReloadServiceStatus::TargetUnavailable,
				TEXT("live_reload_target_unavailable"),
				TEXT("actor_destroyed_during_build"),
				TEXT("The fixed Project C# Auto Live Reload Actor was destroyed during report binding."),
				TEXT("select another Actor and restart Project C# Auto Live Reload"));
			LastResult.TargetActorPath = FixedTargetPath;
			LastResult.Request = CompletingRequest;
			LastResult.AsyncProgress = CompletionProgress;
			LastResult.BuildResult = MoveTemp(BuildResult);
			StopInternal(true);
			return false;
		}
	}

	const bool bSucceeded = BuildResult.bSucceeded && !bCanceled;
	if (!Coordinator.CompleteBuild(CompletingRequest, bSucceeded))
	{
		return false;
	}

	LastResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	LastResult.bSucceeded = bSucceeded;
	LastResult.bRunning = true;
	LastResult.Status = bCanceled
		? EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildCanceled
		: bSucceeded
			? EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildSucceeded
			: EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed;
	LastResult.ErrorCategory = bCanceled
		? AsyncResult.ErrorCategory
		: BuildResult.ErrorCategory;
	LastResult.CauseErrorCategory = bCanceled
		? AsyncResult.ErrorCategory
		: BuildResult.CauseErrorCategory;
	LastResult.ErrorMessage = bCanceled
		? AsyncResult.ErrorMessage
		: BuildResult.ErrorMessage;
	LastResult.NextAction = bCanceled
		? AsyncResult.NextAction
		: BuildResult.NextAction;
	LastResult.WorkspaceRoot = ActiveConfig.WorkspaceRoot;
	LastResult.ProfilePath = ActiveConfig.ProfilePath;
	LastResult.TargetActorPath = FixedTargetPath;
	LastResult.Request = CompletingRequest;
	LastResult.Stats = Coordinator.GetStats();
	LastResult.AsyncProgress = CompletionProgress;
	LastResult.BuildResult = MoveTemp(BuildResult);
	return true;
}

bool FAvidScriptEditorCSharpLiveReloadService::IsActiveRequestCurrent() const
{
	const FAvidScriptEditorCSharpLiveReloadCoordinatorStats& Stats =
		Coordinator.GetStats();
	return ActiveBuildJobSerial != 0
		&& Stats.State == EAvidScriptEditorCSharpLiveReloadState::Building
		&& Stats.SessionGeneration == ActiveBuildRequest.SessionGeneration
		&& Stats.ActiveRequestId == ActiveBuildRequest.RequestId;
}

void FAvidScriptEditorCSharpLiveReloadService::ResetActiveBuildJob()
{
	ActiveBuildJob.Reset();
	ActiveBuildRequest =
		FAvidScriptEditorCSharpLiveReloadBuildRequest();
	ActiveBuildJobSerial = 0;
}
