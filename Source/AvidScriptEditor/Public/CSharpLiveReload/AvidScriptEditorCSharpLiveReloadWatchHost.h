#pragma once

#include "CoreMinimal.h"

struct FAvidScriptEditorCSharpLiveReloadChangeBatch
{
	TArray<FString> FilePaths;
	bool bRescanRequired = false;
};

class IAvidScriptEditorCSharpLiveReloadWatchHost
{
public:
	using FOnChangeBatch = TFunction<void(FAvidScriptEditorCSharpLiveReloadChangeBatch&&)>;

	virtual ~IAvidScriptEditorCSharpLiveReloadWatchHost() = default;

	virtual bool Start(
		const FString& WorkspaceRoot,
		FOnChangeBatch OnChangeBatch,
		FString& OutErrorCategory,
		FString& OutErrorMessage) = 0;

	virtual void Stop() = 0;

	virtual bool IsWatching() const = 0;
};
