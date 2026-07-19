#pragma once

#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJob.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadBuildExecutor.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadCoordinator.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadWatchHost.h"

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
struct FAvidScriptEditorCSharpLiveReloadPendingState;

enum class EAvidScriptEditorCSharpLiveReloadServiceStatus : uint8
{
	Unknown,
	Watching,
	Building,
	BuildSucceeded,
	BuildFailed,
	BuildCanceled,
	TargetUnavailable,
	StartFailed,
	Stopped
};

struct FAvidScriptEditorCSharpLiveReloadServiceConfig
{
	FString WorkspaceRoot;
	FString ProfilePath;
	double DebounceSeconds = 0.35;
};

struct FAvidScriptEditorCSharpLiveReloadServiceResult
{
	bool bSucceeded = false;
	bool bRunning = false;
	EAvidScriptEditorCSharpLiveReloadServiceStatus Status =
		EAvidScriptEditorCSharpLiveReloadServiceStatus::Unknown;
	FString ErrorCategory;
	FString CauseErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString WorkspaceRoot;
	FString ProfilePath;
	FString TargetActorPath;
	FAvidScriptEditorCSharpLiveReloadBuildRequest Request;
	FAvidScriptEditorCSharpLiveReloadCoordinatorStats Stats;
	FAvidScriptEditorCSharpAsyncBuildProgress AsyncProgress;
	FAvidScriptEditorCSharpLiveReloadBuildResult BuildResult;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpLiveReloadService
{
public:
	using FCreateBuildJob = TFunction<
		TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob>()>;
	using FApplyReport = TFunction<bool(
		const FString&,
		AActor*,
		FAvidScriptEditorComponentBindingResult&)>;
	using FNowSeconds = TFunction<double()>;

	FAvidScriptEditorCSharpLiveReloadService();

	FAvidScriptEditorCSharpLiveReloadService(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> InWatchHost,
		FCreateBuildJob InCreateBuildJob,
		FApplyReport InApplyReport,
		FNowSeconds InNowSeconds);

	~FAvidScriptEditorCSharpLiveReloadService();

	bool Start(
		const FAvidScriptEditorCSharpLiveReloadServiceConfig& Config,
		AActor* TargetActor,
		FAvidScriptEditorCSharpLiveReloadServiceResult& OutResult);

	void Stop();

	bool Tick();

	bool IsRunning() const;

	const FAvidScriptEditorCSharpLiveReloadCoordinatorStats& GetStats() const;

	const FAvidScriptEditorCSharpLiveReloadServiceResult& GetLastResult() const;

private:
	bool HandleCoreTicker(float DeltaSeconds);
	void DrainPendingChanges(double NowSeconds);
	bool TryStartBuildJob(double NowSeconds);
	bool CompleteActiveBuildJob(
		AActor* FixedTarget,
		IAvidScriptEditorCSharpAsyncBuildJob* ExpectedJob,
		uint64 ExpectedJobSerial);
	bool IsActiveRequestCurrent() const;
	void ResetActiveBuildJob();
	void StopInternal(bool bPreserveLastResult);
	void SetFailure(
		EAvidScriptEditorCSharpLiveReloadServiceStatus Status,
		const FString& ErrorCategory,
		const FString& CauseErrorCategory,
		const FString& ErrorMessage,
		const FString& NextAction);

	TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> WatchHost;
	FCreateBuildJob CreateBuildJob;
	FApplyReport ApplyReport;
	FNowSeconds NowSeconds;
	TSharedPtr<FAvidScriptEditorCSharpLiveReloadPendingState, ESPMode::ThreadSafe> PendingState;
	FAvidScriptEditorCSharpLiveReloadCoordinator Coordinator;
	FAvidScriptEditorCSharpLiveReloadServiceConfig ActiveConfig;
	TWeakObjectPtr<AActor> TargetActor;
	FString FixedTargetPath;
	TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob> ActiveBuildJob;
	FAvidScriptEditorCSharpLiveReloadBuildRequest ActiveBuildRequest;
	uint64 ActiveBuildJobSerial = 0;
	uint64 NextBuildJobSerial = 1;
	FTSTicker::FDelegateHandle CoreTickerHandle;
	FAvidScriptEditorCSharpLiveReloadServiceResult LastResult;
};
