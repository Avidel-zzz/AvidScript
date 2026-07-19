#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.h"

#include "AvidScriptEditorComponentBindingService.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadDirectoryWatchHost.h"

#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

struct FAvidScriptEditorCSharpLiveReloadPendingState
{
	TAtomic<bool> bAccepting = false;
	TQueue<FAvidScriptEditorCSharpLiveReloadChangeBatch, EQueueMode::Mpsc> Queue;
};

namespace
{
FString NormalizeAvidScriptLiveReloadServicePath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}
	return Path;
}

bool IsAvidScriptLiveReloadTargetValid(AActor* Actor)
{
	return IsValid(Actor) && !Actor->IsActorBeingDestroyed();
}
} // namespace

FAvidScriptEditorCSharpLiveReloadService::
	FAvidScriptEditorCSharpLiveReloadService()
	: FAvidScriptEditorCSharpLiveReloadService(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(
			new FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost()),
		[]()
		{
			return FAvidScriptEditorCSharpAsyncBuildJobFactory::Create();
		},
		[](
			const FString& ReportPath,
			AActor* Target,
			FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return FAvidScriptEditorComponentBindingService::
				ApplyCSharpReportToActor(
					ReportPath,
					Target,
					OutResult);
		},
		[]() { return FPlatformTime::Seconds(); })
{
}

FAvidScriptEditorCSharpLiveReloadService::
	FAvidScriptEditorCSharpLiveReloadService(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> InWatchHost,
		FCreateBuildJob InCreateBuildJob,
		FApplyReport InApplyReport,
		FNowSeconds InNowSeconds)
	: WatchHost(MoveTemp(InWatchHost))
	, CreateBuildJob(MoveTemp(InCreateBuildJob))
	, ApplyReport(MoveTemp(InApplyReport))
	, NowSeconds(MoveTemp(InNowSeconds))
{
}

FAvidScriptEditorCSharpLiveReloadService::
	~FAvidScriptEditorCSharpLiveReloadService()
{
	StopInternal(false);
}

bool FAvidScriptEditorCSharpLiveReloadService::Start(
	const FAvidScriptEditorCSharpLiveReloadServiceConfig& Config,
	AActor* InTargetActor,
	FAvidScriptEditorCSharpLiveReloadServiceResult& OutResult)
{
	if (IsRunning())
	{
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::StartFailed,
			TEXT("live_reload_already_running"),
			FString(),
			TEXT("Project C# Auto Live Reload is already running."),
			TEXT("stop the active watcher before starting a different target"));
		OutResult = LastResult;
		return false;
	}

	LastResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	ActiveConfig = Config;
	ActiveConfig.WorkspaceRoot = NormalizeAvidScriptLiveReloadServicePath(
		Config.WorkspaceRoot);
	ActiveConfig.ProfilePath = NormalizeAvidScriptLiveReloadServicePath(
		Config.ProfilePath);
	TargetActor = InTargetActor;
	if (!IsAvidScriptLiveReloadTargetValid(TargetActor.Get()))
	{
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::TargetUnavailable,
			TEXT("live_reload_target_unavailable"),
			TEXT("actor_invalid"),
			TEXT("Project C# Auto Live Reload requires a valid fixed Actor target."),
			TEXT("select a valid Actor, run Build And Bind, then start auto live reload"));
		OutResult = LastResult;
		TargetActor.Reset();
		ActiveConfig = FAvidScriptEditorCSharpLiveReloadServiceConfig();
		return false;
	}
	FixedTargetPath = TargetActor->GetPathName();
	if (!WatchHost || !CreateBuildJob || !ApplyReport || !NowSeconds)
	{
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::StartFailed,
			TEXT("live_reload_service_invalid"),
			TEXT("service_dependency_missing"),
			TEXT("Project C# Auto Live Reload service dependencies are incomplete."),
			TEXT("restart the editor or reload the AvidScriptEditor module"));
		OutResult = LastResult;
		TargetActor.Reset();
		FixedTargetPath.Reset();
		ActiveConfig = FAvidScriptEditorCSharpLiveReloadServiceConfig();
		return false;
	}

	FAvidScriptEditorCSharpLiveReloadCoordinatorConfig CoordinatorConfig;
	CoordinatorConfig.WorkspaceRoot = ActiveConfig.WorkspaceRoot;
	CoordinatorConfig.DebounceSeconds = ActiveConfig.DebounceSeconds;
	FString ErrorCategory;
	FString ErrorMessage;
	if (!Coordinator.Start(CoordinatorConfig, ErrorCategory, ErrorMessage))
	{
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::StartFailed,
			ErrorCategory,
			FString(),
			ErrorMessage,
			TEXT("repair the project C# workspace and retry"));
		OutResult = LastResult;
		TargetActor.Reset();
		FixedTargetPath.Reset();
		ActiveConfig = FAvidScriptEditorCSharpLiveReloadServiceConfig();
		return false;
	}

	PendingState = MakeShared<
		FAvidScriptEditorCSharpLiveReloadPendingState,
		ESPMode::ThreadSafe>();
	PendingState->bAccepting.Store(true);
	const TWeakPtr<
		FAvidScriptEditorCSharpLiveReloadPendingState,
		ESPMode::ThreadSafe> WeakPendingState = PendingState;
	if (!WatchHost->Start(
			ActiveConfig.WorkspaceRoot,
			[WeakPendingState](
				FAvidScriptEditorCSharpLiveReloadChangeBatch&& Batch)
			{
				const TSharedPtr<
					FAvidScriptEditorCSharpLiveReloadPendingState,
					ESPMode::ThreadSafe> State = WeakPendingState.Pin();
				if (State && State->bAccepting.Load())
				{
					State->Queue.Enqueue(MoveTemp(Batch));
				}
			},
			ErrorCategory,
			ErrorMessage))
	{
		PendingState->bAccepting.Store(false);
		PendingState.Reset();
		Coordinator.Stop();
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::StartFailed,
			ErrorCategory.IsEmpty()
				? FString(TEXT("live_reload_watch_registration_failed"))
				: ErrorCategory,
			FString(),
			ErrorMessage.IsEmpty()
				? FString(TEXT("Project C# workspace watch registration failed."))
				: ErrorMessage,
			TEXT("verify DirectoryWatcher support and restart auto live reload"));
		OutResult = LastResult;
		TargetActor.Reset();
		FixedTargetPath.Reset();
		ActiveConfig = FAvidScriptEditorCSharpLiveReloadServiceConfig();
		return false;
	}

	CoreTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(
			this,
			&FAvidScriptEditorCSharpLiveReloadService::HandleCoreTicker));
	LastResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	LastResult.bSucceeded = true;
	LastResult.bRunning = true;
	LastResult.Status =
		EAvidScriptEditorCSharpLiveReloadServiceStatus::Watching;
	LastResult.WorkspaceRoot = ActiveConfig.WorkspaceRoot;
	LastResult.ProfilePath = ActiveConfig.ProfilePath;
	LastResult.TargetActorPath = FixedTargetPath;
	LastResult.Stats = Coordinator.GetStats();
	OutResult = LastResult;
	return true;
}

void FAvidScriptEditorCSharpLiveReloadService::Stop()
{
	StopInternal(false);
}

bool FAvidScriptEditorCSharpLiveReloadService::IsRunning() const
{
	return Coordinator.IsRunning()
		&& WatchHost
		&& WatchHost->IsWatching();
}

const FAvidScriptEditorCSharpLiveReloadCoordinatorStats&
FAvidScriptEditorCSharpLiveReloadService::GetStats() const
{
	return Coordinator.GetStats();
}

const FAvidScriptEditorCSharpLiveReloadServiceResult&
FAvidScriptEditorCSharpLiveReloadService::GetLastResult() const
{
	return LastResult;
}

bool FAvidScriptEditorCSharpLiveReloadService::HandleCoreTicker(
	const float DeltaSeconds)
{
	return Tick();
}

void FAvidScriptEditorCSharpLiveReloadService::DrainPendingChanges(
	const double CurrentSeconds)
{
	if (!PendingState)
	{
		return;
	}

	FAvidScriptEditorCSharpLiveReloadChangeBatch Batch;
	while (PendingState->Queue.Dequeue(Batch))
	{
		if (Batch.bRescanRequired)
		{
			Coordinator.NotifyWorkspaceRescan(CurrentSeconds);
		}
		if (!Batch.FilePaths.IsEmpty())
		{
			Coordinator.NotifyFileChanges(
				Batch.FilePaths,
				CurrentSeconds);
		}
		Batch = FAvidScriptEditorCSharpLiveReloadChangeBatch();
	}
}

void FAvidScriptEditorCSharpLiveReloadService::StopInternal(
	const bool bPreserveLastResult)
{
	const FString PreviousWorkspaceRoot = ActiveConfig.WorkspaceRoot.IsEmpty()
		? LastResult.WorkspaceRoot
		: ActiveConfig.WorkspaceRoot;
	const FString PreviousProfilePath = ActiveConfig.ProfilePath.IsEmpty()
		? LastResult.ProfilePath
		: ActiveConfig.ProfilePath;
	const FString PreviousTargetPath = FixedTargetPath.IsEmpty()
		? LastResult.TargetActorPath
		: FixedTargetPath;
	if (PendingState)
	{
		PendingState->bAccepting.Store(false);
	}
	if (WatchHost)
	{
		WatchHost->Stop();
	}
	if (ActiveBuildJob)
	{
		ActiveBuildJob->Cancel();
	}
	ResetActiveBuildJob();
	if (CoreTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(CoreTickerHandle);
		CoreTickerHandle.Reset();
	}
	Coordinator.Stop();
	if (PendingState)
	{
		FAvidScriptEditorCSharpLiveReloadChangeBatch IgnoredBatch;
		while (PendingState->Queue.Dequeue(IgnoredBatch))
		{
		}
		PendingState.Reset();
	}
	TargetActor.Reset();
	FixedTargetPath.Reset();
	ActiveConfig = FAvidScriptEditorCSharpLiveReloadServiceConfig();

	if (bPreserveLastResult)
	{
		LastResult.bRunning = false;
		LastResult.Stats = Coordinator.GetStats();
		return;
	}

	LastResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	LastResult.bSucceeded = true;
	LastResult.bRunning = false;
	LastResult.Status =
		EAvidScriptEditorCSharpLiveReloadServiceStatus::Stopped;
	LastResult.WorkspaceRoot = PreviousWorkspaceRoot;
	LastResult.ProfilePath = PreviousProfilePath;
	LastResult.TargetActorPath = PreviousTargetPath;
	LastResult.Stats = Coordinator.GetStats();
}

void FAvidScriptEditorCSharpLiveReloadService::SetFailure(
	const EAvidScriptEditorCSharpLiveReloadServiceStatus Status,
	const FString& ErrorCategory,
	const FString& CauseErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction)
{
	LastResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	LastResult.bSucceeded = false;
	LastResult.bRunning = IsRunning();
	LastResult.Status = Status;
	LastResult.ErrorCategory = ErrorCategory;
	LastResult.CauseErrorCategory = CauseErrorCategory;
	LastResult.ErrorMessage = ErrorMessage;
	LastResult.NextAction = NextAction;
	LastResult.WorkspaceRoot = ActiveConfig.WorkspaceRoot;
	LastResult.ProfilePath = ActiveConfig.ProfilePath;
	LastResult.TargetActorPath = FixedTargetPath;
	LastResult.Stats = Coordinator.GetStats();
}
