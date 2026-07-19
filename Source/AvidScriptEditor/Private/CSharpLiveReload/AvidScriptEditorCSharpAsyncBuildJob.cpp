#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJob.h"

#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJobInternal.h"

#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

namespace
{
bool IsAvidScriptCSharpAsyncBuildRunningStage(
	const EAvidScriptEditorCSharpAsyncBuildStage Stage)
{
	return Stage == EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning
		|| Stage == EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning;
}

bool IsAvidScriptCSharpAsyncBuildTerminalStage(
	const EAvidScriptEditorCSharpAsyncBuildStage Stage)
{
	return Stage == EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind
		|| Stage == EAvidScriptEditorCSharpAsyncBuildStage::Failed
		|| Stage == EAvidScriptEditorCSharpAsyncBuildStage::Canceled;
}

FString NormalizeAvidScriptCSharpAsyncBuildPath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}
	return Path;
}
} // namespace

FAvidScriptEditorCSharpAsyncBuildJob::FAvidScriptEditorCSharpAsyncBuildJob(
	TUniquePtr<IAvidScriptEditorCSharpAsyncBuildBackend> InBackend,
	FCreateProcess InCreateProcess,
	FNowSeconds InNowSeconds)
	: Backend(MoveTemp(InBackend))
	, CreateProcess(MoveTemp(InCreateProcess))
	, NowSeconds(MoveTemp(InNowSeconds))
{
}

FAvidScriptEditorCSharpAsyncBuildJob::
	~FAvidScriptEditorCSharpAsyncBuildJob()
{
	Cancel();
	Process.Reset();
	if (Backend)
	{
		Backend->Cleanup();
	}
}

bool FAvidScriptEditorCSharpAsyncBuildJob::Start(
	const FString& ProfilePath)
{
	if (Progress.Stage != EAvidScriptEditorCSharpAsyncBuildStage::Idle
		|| !Backend
		|| !CreateProcess
		|| !NowSeconds)
	{
		return false;
	}

	Result = FAvidScriptEditorCSharpAsyncBuildResult();
	bResultConsumed = false;
	RequestedProfilePath =
		NormalizeAvidScriptCSharpAsyncBuildPath(ProfilePath);
	StartSeconds = NowSeconds();
	Progress = FAvidScriptEditorCSharpAsyncBuildProgress();
	Progress.Stage =
		EAvidScriptEditorCSharpAsyncBuildStage::Preparing;
	return ApplyBackendStep(Backend->Prepare(ProfilePath));
}

void FAvidScriptEditorCSharpAsyncBuildJob::Tick()
{
	if (Progress.Stage ==
		EAvidScriptEditorCSharpAsyncBuildStage::PublishingBindingSlice)
	{
		UpdateElapsedTime();
		if (!bHasPendingBootstrapCompletion)
		{
			FAvidScriptEditorCSharpAsyncBuildResult FailedResult;
			FailedResult.ErrorCategory =
				TEXT("live_reload_build_state_invalid");
			FailedResult.ErrorMessage =
				TEXT("C# asynchronous build lost its bootstrap completion.");
			SetTerminal(
				EAvidScriptEditorCSharpAsyncBuildStage::Failed,
				MoveTemp(FailedResult));
			return;
		}

		FAvidScriptEditorCSharpAsyncBuildBackendStep Step =
			Backend->CompleteInvocation(
				EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning,
				PendingBootstrapInvocation,
				PendingBootstrapSnapshot);
		PendingBootstrapInvocation =
			FAvidScriptEditorCSharpBuildInvocation();
		PendingBootstrapSnapshot =
			FAvidScriptEditorCSharpBuildProcessSnapshot();
		bHasPendingBootstrapCompletion = false;
		ApplyBackendStep(MoveTemp(Step));
		return;
	}

	if (!IsAvidScriptCSharpAsyncBuildRunningStage(Progress.Stage)
		|| !Process)
	{
		return;
	}

	UpdateElapsedTime();
	FAvidScriptEditorCSharpBuildProcessSnapshot Snapshot;
	Process->Poll(Snapshot);
	if (Snapshot.OutputLines.Num() > CurrentProcessOutputLineCount)
	{
		Progress.OutputLineCount +=
			Snapshot.OutputLines.Num() - CurrentProcessOutputLineCount;
		CurrentProcessOutputLineCount = Snapshot.OutputLines.Num();
	}
	if (!Snapshot.LatestOutputLine.IsEmpty())
	{
		Progress.LatestOutputLine = Snapshot.LatestOutputLine;
	}
	Progress.bCancelRequested =
		Progress.bCancelRequested || Snapshot.bCancelRequested;

	if (Snapshot.State ==
		EAvidScriptEditorCSharpBuildProcessState::Running)
	{
		return;
	}
	if (Snapshot.State ==
		EAvidScriptEditorCSharpBuildProcessState::Canceled)
	{
		FAvidScriptEditorCSharpAsyncBuildResult CanceledResult;
		CanceledResult.ErrorCategory =
			TEXT("live_reload_build_canceled");
		CanceledResult.ErrorMessage =
			TEXT("C# asynchronous build was canceled.");
		CanceledResult.NextAction =
			TEXT("save the C# source again to start a new build");
		Process.Reset();
		SetTerminal(
			EAvidScriptEditorCSharpAsyncBuildStage::Canceled,
			MoveTemp(CanceledResult));
		return;
	}
	if (Snapshot.State !=
		EAvidScriptEditorCSharpBuildProcessState::Completed)
	{
		FAvidScriptEditorCSharpAsyncBuildResult FailedResult;
		FailedResult.ErrorCategory =
			TEXT("live_reload_build_process_failed");
		FailedResult.ErrorMessage =
			TEXT("C# asynchronous build process ended in an invalid state.");
		Process.Reset();
		SetTerminal(
			EAvidScriptEditorCSharpAsyncBuildStage::Failed,
			MoveTemp(FailedResult));
		return;
	}

	const EAvidScriptEditorCSharpAsyncBuildStage CompletedStage =
		Progress.Stage;
	if (CompletedStage ==
		EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning)
	{
		PendingBootstrapInvocation = MoveTemp(ActiveInvocation);
		PendingBootstrapSnapshot = MoveTemp(Snapshot);
		bHasPendingBootstrapCompletion = true;
		Process.Reset();
		Progress.Stage =
			EAvidScriptEditorCSharpAsyncBuildStage::PublishingBindingSlice;
		return;
	}
	Process.Reset();
	ApplyBackendStep(Backend->CompleteInvocation(
		CompletedStage,
		ActiveInvocation,
		Snapshot));
}

void FAvidScriptEditorCSharpAsyncBuildJob::Cancel()
{
	if (Progress.Stage ==
		EAvidScriptEditorCSharpAsyncBuildStage::PublishingBindingSlice)
	{
		Progress.bCancelRequested = true;
		bHasPendingBootstrapCompletion = false;
		PendingBootstrapInvocation =
			FAvidScriptEditorCSharpBuildInvocation();
		PendingBootstrapSnapshot =
			FAvidScriptEditorCSharpBuildProcessSnapshot();
		FAvidScriptEditorCSharpAsyncBuildResult CanceledResult;
		CanceledResult.ErrorCategory =
			TEXT("live_reload_build_canceled");
		CanceledResult.ErrorMessage =
			TEXT("C# asynchronous build was canceled.");
		CanceledResult.NextAction =
			TEXT("save the C# source again to start a new build");
		SetTerminal(
			EAvidScriptEditorCSharpAsyncBuildStage::Canceled,
			MoveTemp(CanceledResult));
		return;
	}
	if (Process
		&& IsAvidScriptCSharpAsyncBuildRunningStage(Progress.Stage))
	{
		Progress.bCancelRequested = true;
		Process->Cancel();
	}
}

bool FAvidScriptEditorCSharpAsyncBuildJob::IsFinished() const
{
	return IsAvidScriptCSharpAsyncBuildTerminalStage(Progress.Stage);
}

const FAvidScriptEditorCSharpAsyncBuildProgress&
FAvidScriptEditorCSharpAsyncBuildJob::GetProgress() const
{
	return Progress;
}

bool FAvidScriptEditorCSharpAsyncBuildJob::ConsumeResult(
	FAvidScriptEditorCSharpAsyncBuildResult& OutResult)
{
	if (!IsFinished() || bResultConsumed)
	{
		return false;
	}

	OutResult = Result;
	bResultConsumed = true;
	return true;
}

bool FAvidScriptEditorCSharpAsyncBuildJob::ApplyBackendStep(
	FAvidScriptEditorCSharpAsyncBuildBackendStep&& Step)
{
	if (Step.NextStage ==
			EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning
		|| Step.NextStage ==
			EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning)
	{
		return LaunchInvocation(
			Step.NextStage,
			MoveTemp(Step.Invocation));
	}
	if (Step.NextStage ==
			EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind
		|| Step.NextStage ==
			EAvidScriptEditorCSharpAsyncBuildStage::Failed)
	{
		SetTerminal(Step.NextStage, MoveTemp(Step.Result));
		return Step.NextStage ==
			EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind;
	}

	FAvidScriptEditorCSharpAsyncBuildResult FailedResult;
	FailedResult.ErrorCategory =
		TEXT("live_reload_build_state_invalid");
	FailedResult.ErrorMessage =
		TEXT("C# asynchronous build backend returned an invalid next stage.");
	SetTerminal(
		EAvidScriptEditorCSharpAsyncBuildStage::Failed,
		MoveTemp(FailedResult));
	return false;
}

bool FAvidScriptEditorCSharpAsyncBuildJob::LaunchInvocation(
	const EAvidScriptEditorCSharpAsyncBuildStage Stage,
	FAvidScriptEditorCSharpBuildInvocation&& Invocation)
{
	Process = CreateProcess();
	if (!Process)
	{
		FAvidScriptEditorCSharpAsyncBuildResult FailedResult;
		FailedResult.ErrorCategory =
			TEXT("live_reload_build_process_unavailable");
		FailedResult.ErrorMessage =
			TEXT("C# asynchronous build process factory returned no process.");
		SetTerminal(
			EAvidScriptEditorCSharpAsyncBuildStage::Failed,
			MoveTemp(FailedResult));
		return false;
	}

	ActiveInvocation = MoveTemp(Invocation);
	CurrentProcessOutputLineCount = 0;
	FString LaunchError;
	if (!Process->Launch(ActiveInvocation, LaunchError))
	{
		Process.Reset();
		FAvidScriptEditorCSharpAsyncBuildResult FailedResult;
		FailedResult.ErrorCategory =
			TEXT("live_reload_build_launch_failed");
		FailedResult.ErrorMessage = LaunchError.IsEmpty()
			? FString(TEXT("C# asynchronous build process could not launch."))
			: LaunchError;
		FailedResult.NextAction =
			TEXT("verify PowerShell and the C# build script, then save again");
		SetTerminal(
			EAvidScriptEditorCSharpAsyncBuildStage::Failed,
			MoveTemp(FailedResult));
		return false;
	}

	Progress.Stage = Stage;
	return true;
}

void FAvidScriptEditorCSharpAsyncBuildJob::SetTerminal(
	const EAvidScriptEditorCSharpAsyncBuildStage Stage,
	FAvidScriptEditorCSharpAsyncBuildResult&& InResult)
{
	UpdateElapsedTime();
	Progress.Stage = Stage;
	bHasPendingBootstrapCompletion = false;
	Result = MoveTemp(InResult);
	if (Result.ProfilePath.IsEmpty())
	{
		Result.ProfilePath = RequestedProfilePath;
	}
	Result.bSucceeded =
		Stage == EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind;
	if (Backend)
	{
		Backend->Cleanup();
	}
}

void FAvidScriptEditorCSharpAsyncBuildJob::UpdateElapsedTime()
{
	if (NowSeconds)
	{
		Progress.ElapsedSeconds =
			FMath::Max(0.0, NowSeconds() - StartSeconds);
	}
}

TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob>
FAvidScriptEditorCSharpAsyncBuildJobFactory::Create()
{
	return MakeUnique<FAvidScriptEditorCSharpAsyncBuildJob>(
		CreateAvidScriptEditorCSharpAsyncBuildBackend(),
		[]()
		{
			return TUniquePtr<IAvidScriptEditorCSharpBuildProcess>(
				new FAvidScriptEditorCSharpMonitoredBuildProcess());
		},
		[]()
		{
			return FPlatformTime::Seconds();
		});
}
