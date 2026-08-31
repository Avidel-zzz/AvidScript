#pragma once

#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadWatchHost.h"
#include "GeneratedTypes/AvidScriptEditorGeneratedTypeReloadPolicy.h"

#include "Containers/Ticker.h"
#include "CoreMinimal.h"

struct FAvidScriptEditorGeneratedTypeReloadPendingState;

class AVIDSCRIPTEDITOR_API FAvidScriptEditorGeneratedTypeReloadService final
{
public:
	using FApplyDescriptor = TFunction<bool(
		const FString&,
		EAvidScriptEditorGeneratedTypeReloadClassification,
		FAvidScriptEditorGeneratedTypeReloadServiceResult&)>;

	FAvidScriptEditorGeneratedTypeReloadService();
	FAvidScriptEditorGeneratedTypeReloadService(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> InWatchHost,
		FApplyDescriptor InApplyDescriptor);
	~FAvidScriptEditorGeneratedTypeReloadService();

	bool Start(
		const FString& DescriptorPath,
		FAvidScriptEditorGeneratedTypeReloadServiceResult& OutResult);
	void Stop();
	bool Tick();
	bool IsRunning() const;
	const FAvidScriptEditorGeneratedTypeReloadServiceResult& GetLastResult() const;

private:
	bool HandleCoreTicker(float DeltaSeconds);
	void DrainPendingChanges();
	void ProcessPublishedDescriptor();
	void StopInternal(bool bPreserveLastResult);
	void SetRejected(
		const FString& ErrorCategory,
		const FString& ErrorMessage,
		const FString& NextAction);

	TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost> WatchHost;
	FApplyDescriptor ApplyDescriptor;
	TSharedPtr<FAvidScriptEditorGeneratedTypeReloadPendingState, ESPMode::ThreadSafe>
		PendingState;
	FString ActiveDescriptorPath;
	FString LastProcessedPackageId;
	FTSTicker::FDelegateHandle CoreTickerHandle;
	FAvidScriptEditorGeneratedTypeReloadStats Stats;
	FAvidScriptEditorGeneratedTypeReloadServiceResult LastResult;
};
