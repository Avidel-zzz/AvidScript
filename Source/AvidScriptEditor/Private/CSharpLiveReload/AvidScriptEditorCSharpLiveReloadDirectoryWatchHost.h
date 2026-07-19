#pragma once

#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadWatchHost.h"

#include "CoreMinimal.h"

class IDirectoryWatcher;
struct FAvidScriptEditorCSharpLiveReloadDirectoryWatchState;

class FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost final
	: public IAvidScriptEditorCSharpLiveReloadWatchHost
{
public:
	virtual ~FAvidScriptEditorCSharpLiveReloadDirectoryWatchHost() override;

	virtual bool Start(
		const FString& WorkspaceRoot,
		FOnChangeBatch OnChangeBatch,
		FString& OutErrorCategory,
		FString& OutErrorMessage) override;

	virtual void Stop() override;

	virtual bool IsWatching() const override;

private:
	IDirectoryWatcher* DirectoryWatcher = nullptr;
	FDelegateHandle DirectoryChangedHandle;
	FString WatchedRoot;
	TSharedPtr<FAvidScriptEditorCSharpLiveReloadDirectoryWatchState, ESPMode::ThreadSafe> WatchState;
};
