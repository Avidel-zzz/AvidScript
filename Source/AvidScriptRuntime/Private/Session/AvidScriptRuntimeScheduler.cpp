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

void FAvidScriptRuntimeScheduler::Attach(FAvidScriptWasmRuntimeInstance& Runtime)
{
	ActiveRuntime = &Runtime;
}

void FAvidScriptRuntimeScheduler::Detach()
{
	ActiveRuntime = nullptr;
}

bool FAvidScriptRuntimeScheduler::Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	return Tick(
		DeltaSeconds,
		OutResult,
		EAvidScriptWasmResultDetail::FullSnapshot);
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

	return ActiveRuntime->Tick(
		DeltaSeconds,
		OutResult,
		ResultDetail);
}

EAvidScriptLifecycleState FAvidScriptRuntimeScheduler::GetLifecycleState() const
{
	return ActiveRuntime != nullptr ? ActiveRuntime->GetLifecycleState() : EAvidScriptLifecycleState::Empty;
}

FString FAvidScriptRuntimeScheduler::GetModuleId() const
{
	return ActiveRuntime != nullptr ? ActiveRuntime->GetModuleId() : FString();
}

int32 FAvidScriptRuntimeScheduler::GetTickCallCount() const
{
	return ActiveRuntime != nullptr ? ActiveRuntime->GetTickCallCount() : 0;
}

int32 FAvidScriptRuntimeScheduler::GetPendingTimerCount() const
{
	return ActiveRuntime != nullptr ? ActiveRuntime->GetPendingTimerCount() : 0;
}

int32 FAvidScriptRuntimeScheduler::GetTimerCallbackCount() const
{
	return ActiveRuntime != nullptr ? ActiveRuntime->GetTimerCallbackCount() : 0;
}

int32 FAvidScriptRuntimeScheduler::GetEventCallbackCount() const
{
	return ActiveRuntime != nullptr ? ActiveRuntime->GetEventCallbackCount() : 0;
}
