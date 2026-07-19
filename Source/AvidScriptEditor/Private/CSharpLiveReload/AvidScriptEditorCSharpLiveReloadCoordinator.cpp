#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadCoordinator.h"

#include "HAL/FileManager.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"

namespace
{
const TCHAR* const AvidScriptCSharpLiveReloadDefaultExtensions[] = {
	TEXT(".cs"),
	TEXT(".csproj"),
	TEXT(".props"),
	TEXT(".targets"),
	TEXT(".json")
};

FString NormalizeAvidScriptCSharpLiveReloadPath(FString Path)
{
	if (Path.IsEmpty())
	{
		return Path;
	}

	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	FPaths::CollapseRelativeDirectories(Path);
	FPaths::RemoveDuplicateSlashes(Path);
	while (Path.Len() > 3 && Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1);
	}
	return Path;
}

FString NormalizeAvidScriptCSharpLiveReloadExtension(FString Extension)
{
	Extension.TrimStartAndEndInline();
	Extension.ToLowerInline();
	if (!Extension.IsEmpty() && !Extension.StartsWith(TEXT(".")))
	{
		Extension.InsertAt(0, TEXT("."));
	}
	return Extension;
}

bool IsAvidScriptCSharpLiveReloadFiniteTime(const double Seconds)
{
	return FMath::IsFinite(Seconds) && Seconds >= 0.0;
}
} // namespace

bool FAvidScriptEditorCSharpLiveReloadCoordinator::Start(
	const FAvidScriptEditorCSharpLiveReloadCoordinatorConfig& Config,
	FString& OutErrorCategory,
	FString& OutErrorMessage)
{
	OutErrorCategory.Reset();
	OutErrorMessage.Reset();

	const FString WorkspaceRoot = NormalizeAvidScriptCSharpLiveReloadPath(Config.WorkspaceRoot);
	if (WorkspaceRoot.IsEmpty() || !IFileManager::Get().DirectoryExists(*WorkspaceRoot))
	{
		OutErrorCategory = TEXT("live_reload_workspace_invalid");
		OutErrorMessage = FString::Printf(
			TEXT("C# live reload workspace does not exist: %s"),
			Config.WorkspaceRoot.IsEmpty() ? TEXT("<empty>") : *Config.WorkspaceRoot);
		return false;
	}
	if (!FMath::IsFinite(Config.DebounceSeconds) || Config.DebounceSeconds < 0.0)
	{
		OutErrorCategory = TEXT("live_reload_debounce_invalid");
		OutErrorMessage = TEXT("C# live reload debounce must be finite and non-negative.");
		return false;
	}

	ActiveConfig = Config;
	ActiveConfig.WorkspaceRoot = WorkspaceRoot;
	RelevantExtensions.Reset();
	auto AddExtension = [this](const FString& Extension)
	{
		const FString NormalizedExtension = NormalizeAvidScriptCSharpLiveReloadExtension(Extension);
		if (!NormalizedExtension.IsEmpty())
		{
			RelevantExtensions.Add(NormalizedExtension);
		}
	};
	if (Config.RelevantExtensions.IsEmpty())
	{
		for (const TCHAR* Extension : AvidScriptCSharpLiveReloadDefaultExtensions)
		{
			AddExtension(Extension);
		}
	}
	else
	{
		for (const FString& Extension : Config.RelevantExtensions)
		{
			AddExtension(Extension);
		}
	}
	if (RelevantExtensions.IsEmpty())
	{
		OutErrorCategory = TEXT("live_reload_extensions_invalid");
		OutErrorMessage = TEXT("C# live reload requires at least one relevant file extension.");
		ActiveConfig = FAvidScriptEditorCSharpLiveReloadCoordinatorConfig();
		return false;
	}

	PendingFiles.Reset();
	ActiveRequest = FAvidScriptEditorCSharpLiveReloadBuildRequest();
	Stats = FAvidScriptEditorCSharpLiveReloadCoordinatorStats();
	Stats.State = EAvidScriptEditorCSharpLiveReloadState::Watching;
	Stats.SessionGeneration = NextSessionGeneration++;
	return true;
}

void FAvidScriptEditorCSharpLiveReloadCoordinator::Stop()
{
	if (Stats.State == EAvidScriptEditorCSharpLiveReloadState::Building)
	{
		++Stats.BuildCanceledCount;
	}

	Stats.State = EAvidScriptEditorCSharpLiveReloadState::Stopped;
	Stats.ActiveRequestId = 0;
	Stats.bPendingBuild = false;
	Stats.PendingDeadlineSeconds = 0.0;
	PendingFiles.Reset();
	ActiveRequest = FAvidScriptEditorCSharpLiveReloadBuildRequest();
	ActiveConfig = FAvidScriptEditorCSharpLiveReloadCoordinatorConfig();
	RelevantExtensions.Reset();
}

bool FAvidScriptEditorCSharpLiveReloadCoordinator::NotifyFileChanges(
	const TArray<FString>& FilePaths,
	const double NowSeconds)
{
	if (!IsRunning() || !IsAvidScriptCSharpLiveReloadFiniteTime(NowSeconds))
	{
		return false;
	}

	TSet<FString> UniqueRelevantPaths;
	for (const FString& FilePath : FilePaths)
	{
		++Stats.ObservedFileCount;
		FString NormalizedPath;
		if (IsRelevantFile(FilePath, NormalizedPath))
		{
			UniqueRelevantPaths.Add(MoveTemp(NormalizedPath));
		}
		else
		{
			++Stats.IgnoredFileCount;
		}
	}
	if (UniqueRelevantPaths.IsEmpty())
	{
		return false;
	}

	RecordRelevantChange(UniqueRelevantPaths.Array(), NowSeconds);
	return true;
}

bool FAvidScriptEditorCSharpLiveReloadCoordinator::NotifyWorkspaceRescan(const double NowSeconds)
{
	if (!IsRunning() || !IsAvidScriptCSharpLiveReloadFiniteTime(NowSeconds))
	{
		return false;
	}

	RecordRelevantChange({ActiveConfig.WorkspaceRoot}, NowSeconds);
	return true;
}

TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest>
FAvidScriptEditorCSharpLiveReloadCoordinator::TryBeginBuild(const double NowSeconds)
{
	if (!IsRunning()
		|| Stats.State == EAvidScriptEditorCSharpLiveReloadState::Building
		|| !Stats.bPendingBuild
		|| !IsAvidScriptCSharpLiveReloadFiniteTime(NowSeconds)
		|| NowSeconds < Stats.PendingDeadlineSeconds)
	{
		return TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest>();
	}

	ActiveRequest = FAvidScriptEditorCSharpLiveReloadBuildRequest();
	ActiveRequest.SessionGeneration = Stats.SessionGeneration;
	ActiveRequest.RequestId = NextRequestId++;
	ActiveRequest.ChangeGeneration = Stats.ChangeGeneration;
	ActiveRequest.ChangedFiles = PendingFiles.Array();
	ActiveRequest.ChangedFiles.Sort();

	PendingFiles.Reset();
	Stats.bPendingBuild = false;
	Stats.PendingDeadlineSeconds = 0.0;
	Stats.ActiveRequestId = ActiveRequest.RequestId;
	Stats.State = EAvidScriptEditorCSharpLiveReloadState::Building;
	++Stats.BuildStartedCount;
	return ActiveRequest;
}

bool FAvidScriptEditorCSharpLiveReloadCoordinator::CompleteBuild(
	const FAvidScriptEditorCSharpLiveReloadBuildRequest& Request,
	const bool bSucceeded)
{
	if (Stats.State != EAvidScriptEditorCSharpLiveReloadState::Building
		|| Request.SessionGeneration != ActiveRequest.SessionGeneration
		|| Request.RequestId != ActiveRequest.RequestId
		|| Request.ChangeGeneration != ActiveRequest.ChangeGeneration)
	{
		return false;
	}

	if (bSucceeded)
	{
		++Stats.BuildSucceededCount;
	}
	else
	{
		++Stats.BuildFailedCount;
	}

	Stats.ActiveRequestId = 0;
	ActiveRequest = FAvidScriptEditorCSharpLiveReloadBuildRequest();
	Stats.State = Stats.bPendingBuild
		? EAvidScriptEditorCSharpLiveReloadState::Debouncing
		: EAvidScriptEditorCSharpLiveReloadState::Watching;
	return true;
}

bool FAvidScriptEditorCSharpLiveReloadCoordinator::IsRunning() const
{
	return Stats.State != EAvidScriptEditorCSharpLiveReloadState::Stopped;
}

const FAvidScriptEditorCSharpLiveReloadCoordinatorStats&
FAvidScriptEditorCSharpLiveReloadCoordinator::GetStats() const
{
	return Stats;
}

bool FAvidScriptEditorCSharpLiveReloadCoordinator::IsRelevantFile(
	const FString& FilePath,
	FString& OutNormalizedPath) const
{
	OutNormalizedPath = NormalizeAvidScriptCSharpLiveReloadPath(FilePath);
	if (OutNormalizedPath.IsEmpty())
	{
		return false;
	}

	const FString WorkspacePrefix = ActiveConfig.WorkspaceRoot + TEXT("/");
	if (!OutNormalizedPath.StartsWith(WorkspacePrefix, ESearchCase::IgnoreCase))
	{
		return false;
	}

	FString Extension = FPaths::GetExtension(OutNormalizedPath, true);
	Extension.ToLowerInline();
	return RelevantExtensions.Contains(Extension);
}

void FAvidScriptEditorCSharpLiveReloadCoordinator::RecordRelevantChange(
	const TArray<FString>& NormalizedPaths,
	const double NowSeconds)
{
	++Stats.ChangeGeneration;
	++Stats.RelevantChangeBatchCount;
	if (Stats.State == EAvidScriptEditorCSharpLiveReloadState::Building)
	{
		++Stats.CoalescedChangeBatchCount;
	}

	for (const FString& Path : NormalizedPaths)
	{
		PendingFiles.Add(Path);
	}
	Stats.bPendingBuild = true;
	Stats.PendingDeadlineSeconds = NowSeconds + ActiveConfig.DebounceSeconds;
	if (Stats.State != EAvidScriptEditorCSharpLiveReloadState::Building)
	{
		Stats.State = EAvidScriptEditorCSharpLiveReloadState::Debouncing;
	}
}
