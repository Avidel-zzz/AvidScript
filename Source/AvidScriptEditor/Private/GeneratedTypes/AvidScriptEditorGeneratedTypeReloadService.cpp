#include "GeneratedTypes/AvidScriptEditorGeneratedTypeReloadService.h"

#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadDirectoryWatchHost.h"

#include "Containers/Queue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptGeneratedTypeReload, Log, All);

struct FAvidScriptEditorGeneratedTypeReloadPendingState
{
	TAtomic<bool> bAccepting = false;
	TQueue<FAvidScriptEditorCSharpLiveReloadChangeBatch, EQueueMode::Mpsc> Queue;
};

namespace
{
FString NormalizeGeneratedTypeReloadPath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		FPaths::RemoveDuplicateSlashes(Path);
	}
	return Path;
}

const TCHAR* LexToString(
	const EAvidScriptEditorGeneratedTypeReloadClassification Classification)
{
	switch (Classification)
	{
	case EAvidScriptEditorGeneratedTypeReloadClassification::InitialInstall:
		return TEXT("initial_install");
	case EAvidScriptEditorGeneratedTypeReloadClassification::BodyOnly:
		return TEXT("body_only");
	case EAvidScriptEditorGeneratedTypeReloadClassification::NativeRebuildRequired:
		return TEXT("native_rebuild_required");
	default:
		return TEXT("unknown");
	}
}
} // namespace

FAvidScriptEditorGeneratedTypeReloadService::
	FAvidScriptEditorGeneratedTypeReloadService()
	: FAvidScriptEditorGeneratedTypeReloadService(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(
			new FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost()),
		[](const FString& DescriptorPath,
			const EAvidScriptEditorGeneratedTypeReloadClassification Classification,
			FAvidScriptEditorGeneratedTypeReloadServiceResult& OutResult)
		{
			return FAvidScriptEditorGeneratedTypeReloadPolicy::ApplyPublishedDescriptor(
				DescriptorPath,
				Classification,
				OutResult);
		})
{
}

FAvidScriptEditorGeneratedTypeReloadService::
	FAvidScriptEditorGeneratedTypeReloadService(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> InWatchHost,
		FApplyDescriptor InApplyDescriptor)
	: WatchHost(MoveTemp(InWatchHost))
	, ApplyDescriptor(MoveTemp(InApplyDescriptor))
{
}

FAvidScriptEditorGeneratedTypeReloadService::
	~FAvidScriptEditorGeneratedTypeReloadService()
{
	StopInternal(false);
}

bool FAvidScriptEditorGeneratedTypeReloadService::Start(
	const FString& DescriptorPath,
	FAvidScriptEditorGeneratedTypeReloadServiceResult& OutResult)
{
	if (IsRunning())
	{
		LastResult = FAvidScriptEditorGeneratedTypeReloadServiceResult();
		LastResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::StartFailed;
		LastResult.ErrorCategory = TEXT("generated_type_reload_already_running");
		LastResult.ErrorMessage = TEXT(
			"Generated type package reload watcher is already running.");
		LastResult.NextAction = TEXT("stop the active watcher before starting another");
		OutResult = LastResult;
		return false;
	}

	Stats = FAvidScriptEditorGeneratedTypeReloadStats();
	LastResult = FAvidScriptEditorGeneratedTypeReloadServiceResult();
	ActiveDescriptorPath = NormalizeGeneratedTypeReloadPath(DescriptorPath);
	const FString WatchRoot = FPaths::GetPath(ActiveDescriptorPath);
	if (!WatchHost || !ApplyDescriptor || ActiveDescriptorPath.IsEmpty()
		|| !IFileManager::Get().DirectoryExists(*WatchRoot))
	{
		LastResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::StartFailed;
		LastResult.ErrorCategory = TEXT("generated_type_reload_service_invalid");
		LastResult.ErrorMessage = FString::Printf(
			TEXT("Generated type reload watcher cannot use descriptor path: %s"),
			ActiveDescriptorPath.IsEmpty() ? TEXT("<empty>") : *ActiveDescriptorPath);
		LastResult.NextAction = TEXT(
			"verify the AvidScriptGenerated source directory and reload the Editor module");
		OutResult = LastResult;
		ActiveDescriptorPath.Reset();
		return false;
	}

	LastProcessedPackageId.Reset();
	if (FPaths::FileExists(ActiveDescriptorPath))
	{
		FString ErrorCategory;
		FString ErrorMessage;
		if (!FAvidScriptEditorGeneratedTypeReloadPolicy::ReadPackageId(
				ActiveDescriptorPath,
				LastProcessedPackageId,
				ErrorCategory,
				ErrorMessage))
		{
			LastResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::StartFailed;
			LastResult.ErrorCategory = ErrorCategory;
			LastResult.ErrorMessage = ErrorMessage;
			LastResult.NextAction = TEXT(
				"repair or remove the invalid generated package descriptor, then restart the watcher");
			OutResult = LastResult;
			ActiveDescriptorPath.Reset();
			return false;
		}
	}

	PendingState = MakeShared<
		FAvidScriptEditorGeneratedTypeReloadPendingState,
		ESPMode::ThreadSafe>();
	PendingState->bAccepting.Store(true);
	const TWeakPtr<
		FAvidScriptEditorGeneratedTypeReloadPendingState,
		ESPMode::ThreadSafe> WeakPendingState = PendingState;
	FString ErrorCategory;
	FString ErrorMessage;
	if (!WatchHost->Start(
			WatchRoot,
			[WeakPendingState](FAvidScriptEditorCSharpLiveReloadChangeBatch&& Batch)
			{
				const TSharedPtr<
					FAvidScriptEditorGeneratedTypeReloadPendingState,
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
		LastResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::StartFailed;
		LastResult.ErrorCategory = ErrorCategory.IsEmpty()
			? FString(TEXT("generated_type_reload_watch_registration_failed"))
			: ErrorCategory;
		LastResult.ErrorMessage = ErrorMessage.IsEmpty()
			? FString(TEXT("Generated type package directory watch registration failed."))
			: ErrorMessage;
		LastResult.NextAction = TEXT(
			"verify DirectoryWatcher support and reload the AvidScriptEditor module");
		OutResult = LastResult;
		ActiveDescriptorPath.Reset();
		LastProcessedPackageId.Reset();
		return false;
	}

	CoreTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(
			this,
			&FAvidScriptEditorGeneratedTypeReloadService::HandleCoreTicker));
	LastResult.bSucceeded = true;
	LastResult.bRunning = true;
	LastResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::Watching;
	LastResult.DescriptorPath = ActiveDescriptorPath;
	LastResult.DescriptorIdentity.PackageId = LastProcessedPackageId;
	LastResult.Stats = Stats;
	OutResult = LastResult;
	return true;
}

void FAvidScriptEditorGeneratedTypeReloadService::Stop()
{
	StopInternal(false);
}

bool FAvidScriptEditorGeneratedTypeReloadService::Tick()
{
	if (!IsRunning())
	{
		return false;
	}
	DrainPendingChanges();
	return true;
}

bool FAvidScriptEditorGeneratedTypeReloadService::IsRunning() const
{
	return WatchHost && WatchHost->IsWatching() && CoreTickerHandle.IsValid();
}

const FAvidScriptEditorGeneratedTypeReloadServiceResult&
	FAvidScriptEditorGeneratedTypeReloadService::GetLastResult() const
{
	return LastResult;
}

bool FAvidScriptEditorGeneratedTypeReloadService::HandleCoreTicker(float DeltaSeconds)
{
	(void)DeltaSeconds;
	return Tick();
}

void FAvidScriptEditorGeneratedTypeReloadService::DrainPendingChanges()
{
	check(IsInGameThread());
	if (!PendingState)
	{
		return;
	}

	bool bDescriptorChanged = false;
	FAvidScriptEditorCSharpLiveReloadChangeBatch Batch;
	while (PendingState->Queue.Dequeue(Batch))
	{
		++Stats.ObservedBatchCount;
		if (Batch.bRescanRequired)
		{
			bDescriptorChanged = true;
		}
		for (const FString& FilePath : Batch.FilePaths)
		{
			if (NormalizeGeneratedTypeReloadPath(FilePath) == ActiveDescriptorPath)
			{
				bDescriptorChanged = true;
			}
		}
	}
	if (bDescriptorChanged)
	{
		ProcessPublishedDescriptor();
	}
}

void FAvidScriptEditorGeneratedTypeReloadService::ProcessPublishedDescriptor()
{
	FAvidScriptEditorGeneratedTypeDescriptorIdentity Identity;
	FString ErrorCategory;
	FString ErrorMessage;
	if (!FAvidScriptEditorGeneratedTypeReloadPolicy::ReadDescriptorIdentity(
			ActiveDescriptorPath,
			Identity,
			ErrorCategory,
			ErrorMessage))
	{
		SetRejected(
			ErrorCategory,
			ErrorMessage,
			TEXT("wait for the atomic descriptor publish or rebuild the generated type package"));
		return;
	}
	if (Identity.PackageId == LastProcessedPackageId)
	{
		++Stats.DuplicateCount;
		LastResult = FAvidScriptEditorGeneratedTypeReloadServiceResult();
		LastResult.bSucceeded = true;
		LastResult.bRunning = true;
		LastResult.Status =
			EAvidScriptEditorGeneratedTypeReloadStatus::DuplicateIgnored;
		LastResult.DescriptorPath = ActiveDescriptorPath;
		LastResult.DescriptorIdentity = Identity;
		LastResult.Stats = Stats;
		return;
	}

	const bool bInitialInstall = Identity.Classification ==
		EAvidScriptEditorGeneratedTypeReloadClassification::InitialInstall;
	if ((bInitialInstall && !LastProcessedPackageId.IsEmpty())
		|| (!bInitialInstall
			&& Identity.PreviousPackageId != LastProcessedPackageId))
	{
		SetRejected(
			TEXT("generated_type_reload_chain_mismatch"),
			FString::Printf(
				TEXT("Generated type package chain expected previous=%s but active=%s."),
				Identity.PreviousPackageId.IsEmpty()
					? TEXT("<none>")
					: *Identity.PreviousPackageId,
				LastProcessedPackageId.IsEmpty()
					? TEXT("<none>")
					: *LastProcessedPackageId),
			TEXT("restart the Editor to install the latest complete generated package"));
		return;
	}

	FAvidScriptEditorGeneratedTypeReloadServiceResult ApplyResult;
	const bool bApplied = ApplyDescriptor(
		ActiveDescriptorPath,
		Identity.Classification,
		ApplyResult);
	ApplyResult.bRunning = true;
	ApplyResult.DescriptorPath = ActiveDescriptorPath;
	ApplyResult.DescriptorIdentity = Identity;
	if (!bApplied || !ApplyResult.bSucceeded)
	{
		++Stats.RejectedCount;
		ApplyResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::Rejected;
		ApplyResult.Stats = Stats;
		LastResult = MoveTemp(ApplyResult);
		UE_LOG(
			LogAvidScriptGeneratedTypeReload,
			Warning,
			TEXT("Generated type package update rejected: category=%s details=%s"),
			*LastResult.ErrorCategory,
			*LastResult.ErrorMessage);
		return;
	}

	LastProcessedPackageId = Identity.PackageId;
	if (ApplyResult.Status ==
		EAvidScriptEditorGeneratedTypeReloadStatus::NativeRebuildRequired)
	{
		++Stats.NativeRebuildRequiredCount;
		UE_LOG(
			LogAvidScriptGeneratedTypeReload,
			Warning,
			TEXT("Generated type package requires native rebuild: package=%s action=%s"),
			*Identity.PackageId,
			*ApplyResult.NextAction);
	}
	else
	{
		++Stats.AppliedCount;
		UE_LOG(
			LogAvidScriptGeneratedTypeReload,
			Display,
			TEXT("Generated type package update applied: package=%s classification=%s"),
			*Identity.PackageId,
			LexToString(Identity.Classification));
	}
	ApplyResult.Stats = Stats;
	LastResult = MoveTemp(ApplyResult);
}

void FAvidScriptEditorGeneratedTypeReloadService::StopInternal(
	const bool bPreserveLastResult)
{
	if (PendingState)
	{
		PendingState->bAccepting.Store(false);
	}
	if (CoreTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(CoreTickerHandle);
		CoreTickerHandle.Reset();
	}
	if (WatchHost)
	{
		WatchHost->Stop();
	}
	if (PendingState)
	{
		FAvidScriptEditorCSharpLiveReloadChangeBatch IgnoredBatch;
		while (PendingState->Queue.Dequeue(IgnoredBatch))
		{
		}
		PendingState.Reset();
	}
	ActiveDescriptorPath.Reset();
	LastProcessedPackageId.Reset();
	if (!bPreserveLastResult)
	{
		LastResult = FAvidScriptEditorGeneratedTypeReloadServiceResult();
		LastResult.bSucceeded = true;
		LastResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::Stopped;
		LastResult.Stats = Stats;
	}
}

void FAvidScriptEditorGeneratedTypeReloadService::SetRejected(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction)
{
	++Stats.RejectedCount;
	LastResult = FAvidScriptEditorGeneratedTypeReloadServiceResult();
	LastResult.bRunning = IsRunning();
	LastResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::Rejected;
	LastResult.ErrorCategory = ErrorCategory;
	LastResult.ErrorMessage = ErrorMessage;
	LastResult.NextAction = NextAction;
	LastResult.DescriptorPath = ActiveDescriptorPath;
	LastResult.Stats = Stats;
	UE_LOG(
		LogAvidScriptGeneratedTypeReload,
		Warning,
		TEXT("Generated type package update rejected: category=%s details=%s"),
		*ErrorCategory,
		*ErrorMessage);
}
