#pragma once

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
	BuildSucceeded,
	BuildFailed,
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
	FAvidScriptEditorCSharpLiveReloadBuildResult BuildResult;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpLiveReloadService
{
public:
	using FExecuteBuild = TFunction<bool(
		const FString&,
		AActor*,
		FAvidScriptEditorCSharpLiveReloadBuildResult&)>;
	using FNowSeconds = TFunction<double()>;

	FAvidScriptEditorCSharpLiveReloadService();

	FAvidScriptEditorCSharpLiveReloadService(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> InWatchHost,
		FExecuteBuild InExecuteBuild,
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
	void StopInternal(bool bPreserveLastResult);
	void SetFailure(
		EAvidScriptEditorCSharpLiveReloadServiceStatus Status,
		const FString& ErrorCategory,
		const FString& CauseErrorCategory,
		const FString& ErrorMessage,
		const FString& NextAction);

	TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> WatchHost;
	FExecuteBuild ExecuteBuild;
	FNowSeconds NowSeconds;
	TSharedPtr<FAvidScriptEditorCSharpLiveReloadPendingState, ESPMode::ThreadSafe> PendingState;
	FAvidScriptEditorCSharpLiveReloadCoordinator Coordinator;
	FAvidScriptEditorCSharpLiveReloadServiceConfig ActiveConfig;
	TWeakObjectPtr<AActor> TargetActor;
	FTSTicker::FDelegateHandle CoreTickerHandle;
	FAvidScriptEditorCSharpLiveReloadServiceResult LastResult;
};
