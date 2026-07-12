#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptLifecycleState : uint8
{
	Empty,
	Loaded,
	Starting,
	Running,
	Stopping,
	Stopped,
	Faulted
};

struct FAvidScriptLifecycleTransitionResult
{
	bool bSucceeded = false;
	bool bChanged = false;
	EAvidScriptLifecycleState PreviousState = EAvidScriptLifecycleState::Empty;
	EAvidScriptLifecycleState RequestedState = EAvidScriptLifecycleState::Empty;
};

class AVIDSCRIPTCORE_API FAvidScriptLifecycleStateMachine
{
public:
	EAvidScriptLifecycleState GetState() const { return State; }
	bool TryTransition(EAvidScriptLifecycleState RequestedState, FAvidScriptLifecycleTransitionResult& OutResult);
	bool MarkFaulted(FAvidScriptLifecycleTransitionResult& OutResult);
	void Reset();

private:
	static bool IsAllowed(EAvidScriptLifecycleState From, EAvidScriptLifecycleState To);

	EAvidScriptLifecycleState State = EAvidScriptLifecycleState::Empty;
};
