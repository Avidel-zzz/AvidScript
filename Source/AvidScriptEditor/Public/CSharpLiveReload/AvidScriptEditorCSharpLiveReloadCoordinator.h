#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptEditorCSharpLiveReloadState : uint8
{
	Stopped,
	Watching,
	Debouncing,
	Building
};

struct FAvidScriptEditorCSharpLiveReloadCoordinatorConfig
{
	FString WorkspaceRoot;
	double DebounceSeconds = 0.35;
	TArray<FString> RelevantExtensions;
};

struct FAvidScriptEditorCSharpLiveReloadBuildRequest
{
	uint64 SessionGeneration = 0;
	uint64 RequestId = 0;
	uint64 ChangeGeneration = 0;
	TArray<FString> ChangedFiles;
};

struct FAvidScriptEditorCSharpLiveReloadCoordinatorStats
{
	EAvidScriptEditorCSharpLiveReloadState State = EAvidScriptEditorCSharpLiveReloadState::Stopped;
	uint64 SessionGeneration = 0;
	uint64 ChangeGeneration = 0;
	uint64 ActiveRequestId = 0;
	int32 ObservedFileCount = 0;
	int32 IgnoredFileCount = 0;
	int32 RelevantChangeBatchCount = 0;
	int32 CoalescedChangeBatchCount = 0;
	int32 BuildStartedCount = 0;
	int32 BuildSucceededCount = 0;
	int32 BuildFailedCount = 0;
	int32 BuildCanceledCount = 0;
	bool bPendingBuild = false;
	double PendingDeadlineSeconds = 0.0;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpLiveReloadCoordinator
{
public:
	bool Start(
		const FAvidScriptEditorCSharpLiveReloadCoordinatorConfig& Config,
		FString& OutErrorCategory,
		FString& OutErrorMessage);

	void Stop();

	bool NotifyFileChanges(
		const TArray<FString>& FilePaths,
		double NowSeconds);

	bool NotifyWorkspaceRescan(double NowSeconds);

	TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest> TryBeginBuild(double NowSeconds);

	bool CompleteBuild(
		const FAvidScriptEditorCSharpLiveReloadBuildRequest& Request,
		bool bSucceeded);

	bool IsRunning() const;

	const FAvidScriptEditorCSharpLiveReloadCoordinatorStats& GetStats() const;

private:
	bool IsRelevantFile(const FString& FilePath, FString& OutNormalizedPath) const;
	void RecordRelevantChange(const TArray<FString>& NormalizedPaths, double NowSeconds);

	FAvidScriptEditorCSharpLiveReloadCoordinatorConfig ActiveConfig;
	FAvidScriptEditorCSharpLiveReloadCoordinatorStats Stats;
	TSet<FString> RelevantExtensions;
	TSet<FString> PendingFiles;
	FAvidScriptEditorCSharpLiveReloadBuildRequest ActiveRequest;
	uint64 NextRequestId = 1;
	uint64 NextSessionGeneration = 1;
};
