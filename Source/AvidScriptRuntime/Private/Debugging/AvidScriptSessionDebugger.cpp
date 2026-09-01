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

	State = EAvidScriptDebugSessionState::Paused;
	RunMode = EAvidScriptDebugRunMode::Continue;
	ActiveProbeId = ProbeId;
	++PauseSequence;
	return EAvidScriptDebugProbeAction::Pause;
}

bool FAvidScriptSessionDebugger::IsAttached() const
{
	return State != EAvidScriptDebugSessionState::Detached;
}

void FAvidScriptSessionDebugger::Resume(const EAvidScriptDebugRunMode NextMode)
{
	SuppressedProbeId = ActiveProbeId;
	ActiveProbeId = 0;
	State = EAvidScriptDebugSessionState::Running;
	RunMode = NextMode;
}

