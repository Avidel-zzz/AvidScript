#include "AvidScriptLifecycleState.h"

bool FAvidScriptLifecycleStateMachine::TryTransition(
	EAvidScriptLifecycleState RequestedState,
	FAvidScriptLifecycleTransitionResult& OutResult)
{
	OutResult = FAvidScriptLifecycleTransitionResult();
	OutResult.PreviousState = State;
	OutResult.RequestedState = RequestedState;

	if (RequestedState == State)
	{
		OutResult.bSucceeded = true;
		return true;
	}

	if (!IsAllowed(State, RequestedState))
	{
		return false;
	}

	State = RequestedState;
	OutResult.bSucceeded = true;
	OutResult.bChanged = true;
	return true;
}

bool FAvidScriptLifecycleStateMachine::MarkFaulted(FAvidScriptLifecycleTransitionResult& OutResult)
{
	return TryTransition(EAvidScriptLifecycleState::Faulted, OutResult);
}

void FAvidScriptLifecycleStateMachine::Reset()
{
	State = EAvidScriptLifecycleState::Empty;
}

bool FAvidScriptLifecycleStateMachine::IsAllowed(
	EAvidScriptLifecycleState From,
	EAvidScriptLifecycleState To)
{
	if (To == EAvidScriptLifecycleState::Faulted)
	{
		return From != EAvidScriptLifecycleState::Empty && From != EAvidScriptLifecycleState::Stopped;
	}

	switch (From)
	{
	case EAvidScriptLifecycleState::Empty:
		return To == EAvidScriptLifecycleState::Loaded;
	case EAvidScriptLifecycleState::Loaded:
		return To == EAvidScriptLifecycleState::Starting;
	case EAvidScriptLifecycleState::Starting:
		return To == EAvidScriptLifecycleState::Running;
	case EAvidScriptLifecycleState::Running:
		return To == EAvidScriptLifecycleState::Stopping;
	case EAvidScriptLifecycleState::Stopping:
		return To == EAvidScriptLifecycleState::Stopped;
	case EAvidScriptLifecycleState::Stopped:
	case EAvidScriptLifecycleState::Faulted:
	default:
		return false;
	}
}
