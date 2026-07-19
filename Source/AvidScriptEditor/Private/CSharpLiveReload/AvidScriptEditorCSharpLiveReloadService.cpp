#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.h"

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

FAvidScriptEditorCSharpLiveReloadService::FAvidScriptEditorCSharpLiveReloadService()
	: FAvidScriptEditorCSharpLiveReloadService(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(
			new FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost()),
		[Executor = MakeShared<FAvidScriptEditorCSharpLiveReloadBuildExecutor>()](
			const FString& ProfilePath,
			AActor* Target,
			FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult)
		{
			return Executor->Execute(ProfilePath, Target, OutResult);
		},
		[]() { return FPlatformTime::Seconds(); })
{
}

FAvidScriptEditorCSharpLiveReloadService::FAvidScriptEditorCSharpLiveReloadService(
	TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> InWatchHost,
	FExecuteBuild InExecuteBuild,
	FNowSeconds InNowSeconds)
	: WatchHost(MoveTemp(InWatchHost))
	, ExecuteBuild(MoveTemp(InExecuteBuild))
	, NowSeconds(MoveTemp(InNowSeconds))
{
}

FAvidScriptEditorCSharpLiveReloadService::~FAvidScriptEditorCSharpLiveReloadService()
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
	ActiveConfig.WorkspaceRoot = NormalizeAvidScriptLiveReloadServicePath(Config.WorkspaceRoot);
	ActiveConfig.ProfilePath = NormalizeAvidScriptLiveReloadServicePath(Config.ProfilePath);
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
	if (!WatchHost || !ExecuteBuild || !NowSeconds)
	{
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::StartFailed,
			TEXT("live_reload_service_invalid"),
			TEXT("service_dependency_missing"),
			TEXT("Project C# Auto Live Reload service dependencies are incomplete."),
			TEXT("restart the editor or reload the AvidScriptEditor module"));
		OutResult = LastResult;
		TargetActor.Reset();
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
		ActiveConfig = FAvidScriptEditorCSharpLiveReloadServiceConfig();
		return false;
	}

	PendingState = MakeShared<FAvidScriptEditorCSharpLiveReloadPendingState, ESPMode::ThreadSafe>();
	PendingState->bAccepting.Store(true);
	const TWeakPtr<FAvidScriptEditorCSharpLiveReloadPendingState, ESPMode::ThreadSafe> WeakPendingState =
		PendingState;
	if (!WatchHost->Start(
			ActiveConfig.WorkspaceRoot,
			[WeakPendingState](FAvidScriptEditorCSharpLiveReloadChangeBatch&& Batch)
			{
				const TSharedPtr<FAvidScriptEditorCSharpLiveReloadPendingState, ESPMode::ThreadSafe> State =
					WeakPendingState.Pin();
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
			ErrorCategory.IsEmpty() ? FString(TEXT("live_reload_watch_registration_failed")) : ErrorCategory,
			FString(),
			ErrorMessage.IsEmpty() ? FString(TEXT("Project C# workspace watch registration failed.")) : ErrorMessage,
			TEXT("verify DirectoryWatcher support and restart auto live reload"));
		OutResult = LastResult;
		TargetActor.Reset();
		ActiveConfig = FAvidScriptEditorCSharpLiveReloadServiceConfig();
		return false;
	}

	CoreTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FAvidScriptEditorCSharpLiveReloadService::HandleCoreTicker));
	LastResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	LastResult.bSucceeded = true;
	LastResult.bRunning = true;
	LastResult.Status = EAvidScriptEditorCSharpLiveReloadServiceStatus::Watching;
	LastResult.WorkspaceRoot = ActiveConfig.WorkspaceRoot;
	LastResult.ProfilePath = ActiveConfig.ProfilePath;
	LastResult.TargetActorPath = TargetActor->GetPathName();
	LastResult.Stats = Coordinator.GetStats();
	OutResult = LastResult;
	return true;
}

void FAvidScriptEditorCSharpLiveReloadService::Stop()
{
	StopInternal(false);
}

bool FAvidScriptEditorCSharpLiveReloadService::Tick()
{
	if (!IsRunning())
	{
		return false;
	}

	const double BeforeBuildSeconds = NowSeconds();
	DrainPendingChanges(BeforeBuildSeconds);
	AActor* FixedTarget = TargetActor.Get();
	if (!IsAvidScriptLiveReloadTargetValid(FixedTarget))
	{
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::TargetUnavailable,
			TEXT("live_reload_target_unavailable"),
			TEXT("actor_destroyed"),
			TEXT("The fixed Project C# Auto Live Reload Actor was destroyed."),
			TEXT("select another Actor and restart Project C# Auto Live Reload"));
		StopInternal(true);
		return false;
	}

	const TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest> Request =
		Coordinator.TryBeginBuild(BeforeBuildSeconds);
	if (!Request.IsSet())
	{
		return true;
	}

	FAvidScriptEditorCSharpLiveReloadBuildResult BuildResult;
	const FString FixedTargetPath = FixedTarget->GetPathName();
	const bool bBuildSucceeded = ExecuteBuild(
		ActiveConfig.ProfilePath,
		FixedTarget,
		BuildResult);
	DrainPendingChanges(NowSeconds());
	if (!Coordinator.CompleteBuild(*Request, bBuildSucceeded && BuildResult.bSucceeded))
	{
		return IsRunning();
	}
	if (!IsAvidScriptLiveReloadTargetValid(FixedTarget))
	{
		SetFailure(
			EAvidScriptEditorCSharpLiveReloadServiceStatus::TargetUnavailable,
			TEXT("live_reload_target_unavailable"),
			TEXT("actor_destroyed_during_build"),
			TEXT("The fixed Project C# Auto Live Reload Actor was destroyed during build or binding."),
			TEXT("select another Actor and restart Project C# Auto Live Reload"));
		LastResult.TargetActorPath = FixedTargetPath;
		StopInternal(true);
		return false;
	}

	LastResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	LastResult.bSucceeded = bBuildSucceeded && BuildResult.bSucceeded;
	LastResult.bRunning = true;
	LastResult.Status = LastResult.bSucceeded
		? EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildSucceeded
		: EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed;
	LastResult.ErrorCategory = BuildResult.ErrorCategory;
	LastResult.CauseErrorCategory = BuildResult.CauseErrorCategory;
	LastResult.ErrorMessage = BuildResult.ErrorMessage;
	LastResult.NextAction = BuildResult.NextAction;
	LastResult.WorkspaceRoot = ActiveConfig.WorkspaceRoot;
	LastResult.ProfilePath = ActiveConfig.ProfilePath;
	LastResult.TargetActorPath = FixedTargetPath;
	LastResult.Request = *Request;
	LastResult.Stats = Coordinator.GetStats();
	LastResult.BuildResult = MoveTemp(BuildResult);
	return true;
}

bool FAvidScriptEditorCSharpLiveReloadService::IsRunning() const
{
	return Coordinator.IsRunning() && WatchHost && WatchHost->IsWatching();
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

bool FAvidScriptEditorCSharpLiveReloadService::HandleCoreTicker(const float DeltaSeconds)
{
	return Tick();
}

void FAvidScriptEditorCSharpLiveReloadService::DrainPendingChanges(const double CurrentSeconds)
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
			Coordinator.NotifyFileChanges(Batch.FilePaths, CurrentSeconds);
		}
		Batch = FAvidScriptEditorCSharpLiveReloadChangeBatch();
	}
}

void FAvidScriptEditorCSharpLiveReloadService::StopInternal(const bool bPreserveLastResult)
{
	const FString PreviousWorkspaceRoot = ActiveConfig.WorkspaceRoot;
	const FString PreviousProfilePath = ActiveConfig.ProfilePath;
	const FString PreviousTargetPath = LastResult.TargetActorPath;
	if (PendingState)
	{
		PendingState->bAccepting.Store(false);
	}
	if (WatchHost)
	{
		WatchHost->Stop();
	}
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
	LastResult.Status = EAvidScriptEditorCSharpLiveReloadServiceStatus::Stopped;
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
	if (TargetActor.IsValid())
	{
		LastResult.TargetActorPath = TargetActor->GetPathName();
	}
	LastResult.Stats = Coordinator.GetStats();
}
