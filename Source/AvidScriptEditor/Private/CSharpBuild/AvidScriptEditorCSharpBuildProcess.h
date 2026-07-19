#pragma once

#include "CSharpBuild/AvidScriptEditorCSharpBuildInvoker.h"

#include "CoreMinimal.h"

enum class EAvidScriptEditorCSharpBuildProcessState : uint8
{
	Idle,
	Running,
	Completed,
	Canceled,
	LaunchFailed
};

struct FAvidScriptEditorCSharpBuildProcessSnapshot
{
	EAvidScriptEditorCSharpBuildProcessState State =
		EAvidScriptEditorCSharpBuildProcessState::Idle;
	int32 ProcessExitCode = INDEX_NONE;
	TArray<FString> OutputLines;
	FString LatestOutputLine;
	FString Stdout;
	bool bCancelRequested = false;
	double ElapsedSeconds = 0.0;
};

class IAvidScriptEditorCSharpBuildProcess
{
public:
	virtual ~IAvidScriptEditorCSharpBuildProcess() = default;

	virtual bool Launch(
		const FAvidScriptEditorCSharpBuildInvocation& Invocation,
		FString& OutErrorMessage) = 0;

	virtual bool Poll(FAvidScriptEditorCSharpBuildProcessSnapshot& OutSnapshot) = 0;

	virtual void Cancel() = 0;

	virtual bool IsRunning() const = 0;
};

class FAvidScriptEditorCSharpMonitoredBuildProcess final
	: public IAvidScriptEditorCSharpBuildProcess
{
public:
	FAvidScriptEditorCSharpMonitoredBuildProcess();
	virtual ~FAvidScriptEditorCSharpMonitoredBuildProcess() override;

	virtual bool Launch(
		const FAvidScriptEditorCSharpBuildInvocation& Invocation,
		FString& OutErrorMessage) override;

	virtual bool Poll(FAvidScriptEditorCSharpBuildProcessSnapshot& OutSnapshot) override;

	virtual void Cancel() override;

	virtual bool IsRunning() const override;

private:
	struct FPendingState;

	void CollectTrailingOutput();

	TSharedPtr<FPendingState, ESPMode::ThreadSafe> PendingState;
	TUniquePtr<class FMonitoredProcess> Process;
	uint64 LastPolledGeneration = 0;
	bool bTrailingOutputCollected = false;
};
