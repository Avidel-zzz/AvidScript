#include "AvidScriptRuntimeScheduler.h"

namespace
{
void SetSchedulerStateFailure(
	const FAvidScriptWasmRuntimeInstance* Runtime,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = Runtime != nullptr ? Runtime->GetModuleId() : FString();
	OutResult.ExportName = TEXT("avid_on_tick");
	OutResult.ErrorCategory = TEXT("invalid_state");
	OutResult.NextAction = TEXT("attach a Running runtime to the session scheduler before ticking");
	OutResult.ErrorMessage = TEXT("AvidScript scheduler rejected Tick because no Running runtime is attached.");
}
} // namespace

void FAvidScriptRuntimeScheduler::Attach(FAvidScriptWasmRuntimeInstance& Runtime, const FAvidScriptWasmHostContext* InstanceContext)
{
	ActiveRuntime = &Runtime;
	ActiveInstanceContext = InstanceContext;
}

void FAvidScriptRuntimeScheduler::Detach()
{
	ActiveRuntime = nullptr;
	ActiveInstanceContext = nullptr;
}

bool FAvidScriptRuntimeScheduler::Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	return Tick(
		DeltaSeconds,
		OutResult,
		EAvidScriptWasmResultDetail::FullSnapshot);
}

bool FAvidScriptRuntimeScheduler::TickHot(
	const float DeltaSeconds,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	if (ActiveRuntime == nullptr)
	{
		SetSchedulerStateFailure(ActiveRuntime, OutFailure);
		return false;
	}
	return ActiveInstanceContext
		? ActiveRuntime->TickInContext(*ActiveInstanceContext, DeltaSeconds, OutFailure, EAvidScriptWasmResultDetail::HotFailureOnly)
		: ActiveRuntime->TickHot(DeltaSeconds, OutFailure);
}

bool FAvidScriptRuntimeScheduler::Tick(
	const float DeltaSeconds,
	FAvidScriptWasmSmokeResult& OutResult,
	const EAvidScriptWasmResultDetail ResultDetail)
{
	if (ActiveRuntime == nullptr)
	{
		SetSchedulerStateFailure(ActiveRuntime, OutResult);
		return false;
	}

	if (ActiveInstanceContext) return ActiveRuntime->TickInContext(*ActiveInstanceContext, DeltaSeconds, OutResult, ResultDetail);
	return ActiveRuntime->Tick(
		DeltaSeconds,
		OutResult,
		ResultDetail);
}

EAvidScriptLifecycleState FAvidScriptRuntimeScheduler::GetLifecycleState() const
{
	if (ActiveInstanceContext)
	{
		const auto& State = ActiveInstanceContext->InstanceExecutionState;
		return !State ? EAvidScriptLifecycleState::Empty
			: State->IsRetired() ? EAvidScriptLifecycleState::Stopped : State->GetLifecycleState();
	}
	return ActiveRuntime != nullptr ? ActiveRuntime->GetLifecycleState() : EAvidScriptLifecycleState::Empty;
}

FString FAvidScriptRuntimeScheduler::GetModuleId() const
{
	return ActiveRuntime != nullptr ? ActiveRuntime->GetModuleId() : FString();
}

int32 FAvidScriptRuntimeScheduler::GetTickCallCount() const
{
	if (ActiveInstanceContext) return ActiveInstanceContext->InstanceExecutionState ? ActiveInstanceContext->InstanceExecutionState->GetTickCallCount() : 0;
	return ActiveRuntime != nullptr ? ActiveRuntime->GetTickCallCount() : 0;
}

int32 FAvidScriptRuntimeScheduler::GetPendingTimerCount() const
{
	if (ActiveInstanceContext) return ActiveInstanceContext->InstanceExecutionState ? ActiveInstanceContext->InstanceExecutionState->GetPendingTimerCount() : 0;
	return ActiveRuntime != nullptr ? ActiveRuntime->GetPendingTimerCount() : 0;
}

int32 FAvidScriptRuntimeScheduler::GetTimerCallbackCount() const
{
	if (ActiveInstanceContext) return ActiveInstanceContext->InstanceExecutionState ? ActiveInstanceContext->InstanceExecutionState->GetTimerCallbackCount() : 0;
	return ActiveRuntime != nullptr ? ActiveRuntime->GetTimerCallbackCount() : 0;
}

int32 FAvidScriptRuntimeScheduler::GetEventCallbackCount() const
{
	if (ActiveInstanceContext) return ActiveInstanceContext->InstanceExecutionState ? ActiveInstanceContext->InstanceExecutionState->GetEventCallbackCount() : 0;
	return ActiveRuntime != nullptr ? ActiveRuntime->GetEventCallbackCount() : 0;
}
