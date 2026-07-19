#include "CSharpBuild/AvidScriptEditorCSharpBuildProcess.h"

#include "HAL/CriticalSection.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/ScopeLock.h"

struct FAvidScriptEditorCSharpMonitoredBuildProcess::FPendingState
{
	mutable FCriticalSection Mutex;
	EAvidScriptEditorCSharpBuildProcessState State =
		EAvidScriptEditorCSharpBuildProcessState::Idle;
	int32 ProcessExitCode = INDEX_NONE;
	TArray<FString> OutputLines;
	bool bCancelRequested = false;
	uint64 Generation = 0;
};

namespace
{
template <typename StateType>
void AppendAvidScriptCSharpBuildProcessOutput(
	const TSharedPtr<StateType, ESPMode::ThreadSafe>& State,
	const FString& Line)
{
	if (!State)
	{
		return;
	}

	FScopeLock Lock(&State->Mutex);
	State->OutputLines.Add(Line);
	++State->Generation;
}

template <typename StateType>
void CompleteAvidScriptCSharpBuildProcess(
	const TSharedPtr<StateType, ESPMode::ThreadSafe>& State,
	const EAvidScriptEditorCSharpBuildProcessState TerminalState,
	const int32 ProcessExitCode)
{
	if (!State)
	{
		return;
	}

	FScopeLock Lock(&State->Mutex);
	if (State->State != EAvidScriptEditorCSharpBuildProcessState::Running)
	{
		return;
	}
	State->State = TerminalState;
	State->ProcessExitCode = ProcessExitCode;
	++State->Generation;
}
} // namespace

FAvidScriptEditorCSharpMonitoredBuildProcess::FAvidScriptEditorCSharpMonitoredBuildProcess()
	: PendingState(MakeShared<FPendingState, ESPMode::ThreadSafe>())
{
}

FAvidScriptEditorCSharpMonitoredBuildProcess::~FAvidScriptEditorCSharpMonitoredBuildProcess()
{
	Cancel();
	Process.Reset();
	PendingState.Reset();
}

bool FAvidScriptEditorCSharpMonitoredBuildProcess::Launch(
	const FAvidScriptEditorCSharpBuildInvocation& Invocation,
	FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	if (Process)
	{
		OutErrorMessage = TEXT("C# build process instances can only launch one invocation.");
		return false;
	}
	if (Invocation.ExecutablePath.IsEmpty())
	{
		OutErrorMessage = TEXT("C# build invocation executable path is empty.");
		return false;
	}

	{
		FScopeLock Lock(&PendingState->Mutex);
		PendingState->State = EAvidScriptEditorCSharpBuildProcessState::Running;
		PendingState->ProcessExitCode = INDEX_NONE;
		PendingState->OutputLines.Reset();
		PendingState->bCancelRequested = false;
		++PendingState->Generation;
	}
	LastPolledGeneration = 0;
	bTrailingOutputCollected = false;

	Process = MakeUnique<FMonitoredProcess>(
		Invocation.ExecutablePath,
		Invocation.Parameters,
		Invocation.WorkingDirectory,
		true,
		true);
	const TWeakPtr<FPendingState, ESPMode::ThreadSafe> WeakState = PendingState;
	Process->OnOutput().BindLambda(
		[WeakState](const FString& Line)
		{
			AppendAvidScriptCSharpBuildProcessOutput(WeakState.Pin(), Line);
		});
	Process->OnCompleted().BindLambda(
		[WeakState](const int32 ProcessExitCode)
		{
			CompleteAvidScriptCSharpBuildProcess(
				WeakState.Pin(),
				EAvidScriptEditorCSharpBuildProcessState::Completed,
				ProcessExitCode);
		});
	Process->OnCanceled().BindLambda(
		[WeakState]()
		{
			CompleteAvidScriptCSharpBuildProcess(
				WeakState.Pin(),
				EAvidScriptEditorCSharpBuildProcessState::Canceled,
				-1);
		});

	if (!Process->Launch())
	{
		FScopeLock Lock(&PendingState->Mutex);
		PendingState->State = EAvidScriptEditorCSharpBuildProcessState::LaunchFailed;
		PendingState->ProcessExitCode = INDEX_NONE;
		++PendingState->Generation;
		OutErrorMessage = FString::Printf(
			TEXT("C# build process could not launch: %s"),
			*Invocation.ExecutablePath);
		return false;
	}
	return true;
}

bool FAvidScriptEditorCSharpMonitoredBuildProcess::Poll(
	FAvidScriptEditorCSharpBuildProcessSnapshot& OutSnapshot)
{
	if (Process)
	{
		Process->Update();
	}
	CollectTrailingOutput();

	uint64 CurrentGeneration = 0;
	{
		FScopeLock Lock(&PendingState->Mutex);
		OutSnapshot = FAvidScriptEditorCSharpBuildProcessSnapshot();
		OutSnapshot.State = PendingState->State;
		OutSnapshot.ProcessExitCode = PendingState->ProcessExitCode;
		OutSnapshot.OutputLines = PendingState->OutputLines;
		OutSnapshot.bCancelRequested = PendingState->bCancelRequested;
		CurrentGeneration = PendingState->Generation;
	}

	if (!OutSnapshot.OutputLines.IsEmpty())
	{
		OutSnapshot.LatestOutputLine = OutSnapshot.OutputLines.Last();
		OutSnapshot.Stdout = FString::Join(OutSnapshot.OutputLines, LINE_TERMINATOR);
		OutSnapshot.Stdout += LINE_TERMINATOR;
	}
	if (Process)
	{
		OutSnapshot.ElapsedSeconds = Process->GetDuration().GetTotalSeconds();
	}

	const bool bChanged = CurrentGeneration != LastPolledGeneration;
	LastPolledGeneration = CurrentGeneration;
	return bChanged;
}

void FAvidScriptEditorCSharpMonitoredBuildProcess::Cancel()
{
	bool bShouldCancel = false;
	if (PendingState)
	{
		FScopeLock Lock(&PendingState->Mutex);
		if (PendingState->State == EAvidScriptEditorCSharpBuildProcessState::Running
			&& !PendingState->bCancelRequested)
		{
			PendingState->bCancelRequested = true;
			++PendingState->Generation;
			bShouldCancel = true;
		}
	}
	if (bShouldCancel && Process)
	{
		Process->Cancel(true);
	}
}

bool FAvidScriptEditorCSharpMonitoredBuildProcess::IsRunning() const
{
	if (!PendingState)
	{
		return false;
	}

	FScopeLock Lock(&PendingState->Mutex);
	return PendingState->State == EAvidScriptEditorCSharpBuildProcessState::Running;
}

void FAvidScriptEditorCSharpMonitoredBuildProcess::CollectTrailingOutput()
{
	if (bTrailingOutputCollected || !Process || IsRunning())
	{
		return;
	}

	bTrailingOutputCollected = true;
	const FString TrailingOutput = Process->GetFullOutputWithoutDelegate();
	if (!TrailingOutput.IsEmpty())
	{
		AppendAvidScriptCSharpBuildProcessOutput(PendingState, TrailingOutput);
	}
}
