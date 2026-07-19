#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadDirectoryWatchHost.h"

#include "DirectoryWatcherModule.h"
#include "HAL/FileManager.h"
#include "IDirectoryWatcher.h"
#include "Misc/ScopeLock.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
FString NormalizeAvidScriptLiveReloadWatchPath(FString Path)
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
} // namespace

struct FAvidScriptEditorCSharpLiveReloadDirectoryWatchState
{
	void Dispatch(const TArray<FFileChangeData>& FileChanges)
	{
		FAvidScriptEditorCSharpLiveReloadChangeBatch Batch;
		for (const FFileChangeData& Change : FileChanges)
		{
			if (Change.Action == FFileChangeData::FCA_RescanRequired)
			{
				Batch.bRescanRequired = true;
			}
			else if (!Change.Filename.IsEmpty())
			{
				Batch.FilePaths.Add(Change.Filename);
			}
		}
		if (!Batch.bRescanRequired && Batch.FilePaths.IsEmpty())
		{
			return;
		}

		FScopeLock Lock(&Mutex);
		if (bAccepting && ChangeCallback)
		{
			ChangeCallback(MoveTemp(Batch));
		}
	}

	void Close()
	{
		FScopeLock Lock(&Mutex);
		bAccepting = false;
		ChangeCallback = IAvidScriptEditorCSharpLiveReloadWatchHost::FOnChangeBatch();
	}

	FCriticalSection Mutex;
	bool bAccepting = true;
	IAvidScriptEditorCSharpLiveReloadWatchHost::FOnChangeBatch ChangeCallback;
};

FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost::~FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost()
{
	Stop();
}

bool FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost::Start(
	const FString& WorkspaceRoot,
	FOnChangeBatch OnChangeBatch,
	FString& OutErrorCategory,
	FString& OutErrorMessage)
{
	Stop();
	OutErrorCategory.Reset();
	OutErrorMessage.Reset();

	const FString NormalizedRoot = NormalizeAvidScriptLiveReloadWatchPath(WorkspaceRoot);
	if (NormalizedRoot.IsEmpty() || !IFileManager::Get().DirectoryExists(*NormalizedRoot))
	{
		OutErrorCategory = TEXT("live_reload_workspace_invalid");
		OutErrorMessage = FString::Printf(
			TEXT("C# live reload watch root does not exist: %s"),
			WorkspaceRoot.IsEmpty() ? TEXT("<empty>") : *WorkspaceRoot);
		return false;
	}
	if (!OnChangeBatch)
	{
		OutErrorCategory = TEXT("live_reload_watch_callback_invalid");
		OutErrorMessage = TEXT("C# live reload directory callback is not bound.");
		return false;
	}

	FDirectoryWatcherModule* WatcherModule =
		FModuleManager::LoadModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	DirectoryWatcher = WatcherModule != nullptr ? WatcherModule->Get() : nullptr;
	if (DirectoryWatcher == nullptr)
	{
		OutErrorCategory = TEXT("live_reload_watcher_unavailable");
		OutErrorMessage = TEXT("UE DirectoryWatcher is unavailable on this Editor platform.");
		return false;
	}

	WatchedRoot = NormalizedRoot;
	WatchState = MakeShared<
		FAvidScriptEditorCSharpLiveReloadDirectoryWatchState,
		ESPMode::ThreadSafe>();
	WatchState->ChangeCallback = MoveTemp(OnChangeBatch);
	const TSharedRef<FAvidScriptEditorCSharpLiveReloadDirectoryWatchState, ESPMode::ThreadSafe>
		CallbackState = WatchState.ToSharedRef();
	const IDirectoryWatcher::FDirectoryChanged Delegate =
		IDirectoryWatcher::FDirectoryChanged::CreateLambda(
			[CallbackState](const TArray<FFileChangeData>& FileChanges)
			{
				CallbackState->Dispatch(FileChanges);
			});
	if (!DirectoryWatcher->RegisterDirectoryChangedCallback_Handle(
			WatchedRoot,
			Delegate,
			DirectoryChangedHandle))
	{
		OutErrorCategory = TEXT("live_reload_watch_registration_failed");
		OutErrorMessage = FString::Printf(
			TEXT("UE DirectoryWatcher could not register the C# workspace: %s"),
			*WatchedRoot);
		DirectoryWatcher = nullptr;
		WatchedRoot.Reset();
		WatchState->Close();
		WatchState.Reset();
		DirectoryChangedHandle.Reset();
		return false;
	}
	return true;
}

void FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost::Stop()
{
	if (WatchState)
	{
		WatchState->Close();
	}
	if (DirectoryWatcher != nullptr && DirectoryChangedHandle.IsValid() && !WatchedRoot.IsEmpty())
	{
		DirectoryWatcher->UnregisterDirectoryChangedCallback_Handle(
			WatchedRoot,
			DirectoryChangedHandle);
	}
	DirectoryChangedHandle.Reset();
	WatchState.Reset();
	WatchedRoot.Reset();
	DirectoryWatcher = nullptr;
}

bool FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost::IsWatching() const
{
	return DirectoryWatcher != nullptr && DirectoryChangedHandle.IsValid();
}
