#include "Debugging/AvidScriptSessionDebugger.h"

bool FAvidScriptSessionDebugger::Attach(const TConstArrayView<uint64> InBreakpoints)
{
	check(IsInGameThread());
	if (!SetBreakpoints(InBreakpoints))
	{
		return false;
	}

	++Epoch;
	State = EAvidScriptDebugSessionState::Running;
	RunMode = EAvidScriptDebugRunMode::Continue;
	SuppressedProbeId.Reset();
	ActiveProbeId = 0;
	SuspensionToken = 0;
	ResumeRoute = 0;
	SuspensionFrame.Reset();
	return true;
}

void FAvidScriptSessionDebugger::Detach()
{
	check(IsInGameThread());
	++Epoch;
	Breakpoints.Reset();
	State = EAvidScriptDebugSessionState::Detached;
	RunMode = EAvidScriptDebugRunMode::Continue;
	SuppressedProbeId.Reset();
	ActiveProbeId = 0;
	SuspensionToken = 0;
	ResumeRoute = 0;
	SuspensionFrame.Reset();
}

bool FAvidScriptSessionDebugger::SetBreakpoints(
	const TConstArrayView<uint64> InBreakpoints)
{
	check(IsInGameThread());
	if (InBreakpoints.Num() > MaxBreakpointCount)
	{
		return false;
	}

	TSet<uint64> NextBreakpoints;
	NextBreakpoints.Reserve(InBreakpoints.Num());
	for (const uint64 ProbeId : InBreakpoints)
	{
		NextBreakpoints.Add(ProbeId);
	}
	Breakpoints = MoveTemp(NextBreakpoints);
	return true;
}

bool FAvidScriptSessionDebugger::RequestPause()
{
	check(IsInGameThread());
	if (State != EAvidScriptDebugSessionState::Running)
	{
		return false;
	}
	RunMode = EAvidScriptDebugRunMode::PauseNext;
	return true;
}

bool FAvidScriptSessionDebugger::ContinueExecution()
{
	check(IsInGameThread());
	if (State != EAvidScriptDebugSessionState::Paused)
	{
		return false;
	}
	Resume(EAvidScriptDebugRunMode::Continue);
	return true;
}

bool FAvidScriptSessionDebugger::StepInto()
{
	check(IsInGameThread());
	if (State != EAvidScriptDebugSessionState::Paused)
	{
		return false;
	}
	Resume(EAvidScriptDebugRunMode::StepInto);
	return true;
}

void FAvidScriptSessionDebugger::OnRuntimeGenerationChanged()
{
	check(IsInGameThread());
	++Epoch;
	RunMode = EAvidScriptDebugRunMode::Continue;
	SuppressedProbeId.Reset();
	ActiveProbeId = 0;
	SuspensionToken = 0;
	ResumeRoute = 0;
	SuspensionFrame.Reset();
	if (IsAttached())
	{
		State = EAvidScriptDebugSessionState::Running;
	}
}

FAvidScriptDebugSessionSnapshot FAvidScriptSessionDebugger::GetSnapshot() const
{
	check(IsInGameThread());
	FAvidScriptDebugSessionSnapshot Snapshot;
	Snapshot.State = State;
	Snapshot.RunMode = RunMode;
	Snapshot.Epoch = Epoch;
	Snapshot.PauseSequence = PauseSequence;
	Snapshot.ActiveProbeId = ActiveProbeId;
	Snapshot.SuspensionToken = SuspensionToken;
	Snapshot.ResumeRoute = ResumeRoute;
	Snapshot.FrameByteCount = SuspensionFrame.Num();
	Snapshot.BreakpointCount = Breakpoints.Num();
	return Snapshot;
}

EAvidScriptDebugProbeAction FAvidScriptSessionDebugger::EvaluateProbe(
	const uint64 ProbeId)
{
	check(IsInGameThread());
	if (!IsAttached())
	{
		return EAvidScriptDebugProbeAction::Continue;
	}
	if (State == EAvidScriptDebugSessionState::Paused)
	{
		return EAvidScriptDebugProbeAction::Pause;
	}
	if (State == EAvidScriptDebugSessionState::Suspending)
	{
		return EAvidScriptDebugProbeAction::Continue;
	}
	if (State == EAvidScriptDebugSessionState::Resuming)
	{
		return EAvidScriptDebugProbeAction::Abort;
	}

	if (SuppressedProbeId.IsSet())
	{
		const bool bSuppress = SuppressedProbeId.GetValue() == ProbeId;
		SuppressedProbeId.Reset();
		if (bSuppress)
		{
			return EAvidScriptDebugProbeAction::Continue;
		}
	}

	const bool bShouldPause = RunMode != EAvidScriptDebugRunMode::Continue
		|| Breakpoints.Contains(ProbeId);
	if (!bShouldPause)
	{
		return EAvidScriptDebugProbeAction::Continue;
	}

	State = EAvidScriptDebugSessionState::Suspending;
	RunMode = EAvidScriptDebugRunMode::Continue;
	ActiveProbeId = ProbeId;
	return EAvidScriptDebugProbeAction::Pause;
}

int64 FAvidScriptSessionDebugger::CommitSuspension(
	const uint64 ProbeId,
	const uint32 InResumeRoute,
	const TConstArrayView<uint8> FrameBytes)
{
	check(IsInGameThread());
	if (State != EAvidScriptDebugSessionState::Suspending
		|| ActiveProbeId != ProbeId
		|| InResumeRoute == 0
		|| FrameBytes.IsEmpty()
		|| FrameBytes.Num() > MaxFrameByteCount)
	{
		return 0;
	}

	do
	{
		++NextSuspensionToken;
	}
	while (NextSuspensionToken <= 0);
	SuspensionToken = NextSuspensionToken;
	ResumeRoute = InResumeRoute;
	SuspensionFrame = TArray<uint8>(FrameBytes.GetData(), FrameBytes.Num());
	State = EAvidScriptDebugSessionState::Paused;
	++PauseSequence;
	return SuspensionToken;
}

bool FAvidScriptSessionDebugger::ReadSuspensionFrame(
	const int64 InSuspensionToken,
	const TArrayView<uint8> OutFrameBytes)
{
	check(IsInGameThread());
	if (State != EAvidScriptDebugSessionState::Resuming
		|| InSuspensionToken <= 0
		|| InSuspensionToken != SuspensionToken
		|| OutFrameBytes.Num() != SuspensionFrame.Num()
		|| OutFrameBytes.IsEmpty())
	{
		return false;
	}

	FMemory::Memcpy(
		OutFrameBytes.GetData(),
		SuspensionFrame.GetData(),
		SuspensionFrame.Num());
	SuspensionToken = 0;
	ResumeRoute = 0;
	SuspensionFrame.Reset();
	State = EAvidScriptDebugSessionState::Running;
	return true;
}

bool FAvidScriptSessionDebugger::IsExecutionSuspended() const
{
	return State == EAvidScriptDebugSessionState::Suspending
		|| State == EAvidScriptDebugSessionState::Paused
		|| State == EAvidScriptDebugSessionState::Resuming;
}

bool FAvidScriptSessionDebugger::IsAttached() const
{
	return State != EAvidScriptDebugSessionState::Detached;
}

void FAvidScriptSessionDebugger::Resume(const EAvidScriptDebugRunMode NextMode)
{
	SuppressedProbeId = ActiveProbeId;
	ActiveProbeId = 0;
	State = SuspensionToken > 0
		? EAvidScriptDebugSessionState::Resuming
		: EAvidScriptDebugSessionState::Running;
	RunMode = NextMode;
}
