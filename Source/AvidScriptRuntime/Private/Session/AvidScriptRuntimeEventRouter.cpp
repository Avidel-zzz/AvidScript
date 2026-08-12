#include "AvidScriptRuntimeEventRouter.h"

#include "AvidScriptRuntimeScheduler.h"

namespace
{
void SetEventRouterStateFailure(
	const FAvidScriptRuntimeScheduler& Scheduler,
	const TCHAR* ExportName,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = Scheduler.GetModuleId();
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = TEXT("invalid_state");
	OutResult.NextAction = TEXT("attach a Running runtime to the session scheduler before dispatching events");
	OutResult.ErrorMessage = TEXT("AvidScript event router rejected dispatch because no Running runtime is attached.");
}
} // namespace

bool FAvidScriptRuntimeEventRouter::Dispatch(
	int32 EventId,
	float Value,
	FAvidScriptWasmSmokeResult& OutResult)
{
	FAvidScriptWasmRuntimeInstance* Runtime = Scheduler.GetActiveRuntime();
	if (Runtime == nullptr)
	{
		SetEventRouterStateFailure(Scheduler, TEXT("avid_on_event"), OutResult);
		return false;
	}

	return Runtime->DispatchEvent(EventId, Value, OutResult);
}

bool FAvidScriptRuntimeEventRouter::Dispatch(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutResult)
{
	FAvidScriptWasmRuntimeInstance* Runtime = Scheduler.GetActiveRuntime();
	if (Runtime == nullptr)
	{
		SetEventRouterStateFailure(Scheduler, TEXT("avid_on_gameplay_event"), OutResult);
		return false;
	}
	return Runtime->DispatchGameplayEvent(Event, OutResult);
}

bool FAvidScriptRuntimeEventRouter::DispatchHot(
	const int32 EventId,
	const float Value,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	FAvidScriptWasmRuntimeInstance* Runtime = Scheduler.GetActiveRuntime();
	if (Runtime == nullptr)
	{
		SetEventRouterStateFailure(
			Scheduler,
			TEXT("avid_on_event"),
			OutFailure);
		return false;
	}
	return Runtime->DispatchEventHot(EventId, Value, OutFailure);
}

bool FAvidScriptRuntimeEventRouter::DispatchHot(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	FAvidScriptWasmRuntimeInstance* Runtime = Scheduler.GetActiveRuntime();
	if (Runtime == nullptr)
	{
		SetEventRouterStateFailure(
			Scheduler,
			TEXT("avid_on_gameplay_event"),
			OutFailure);
		return false;
	}
	return Runtime->DispatchGameplayEventHot(Event, OutFailure);
}

bool FAvidScriptRuntimeEventRouter::Dispatch(
	const FAvidScriptPreparedDelegateEvent& Event,
	const void* NativeParameters,
	FAvidScriptWasmSmokeResult& OutResult)
{
	FAvidScriptWasmRuntimeInstance* Runtime = Scheduler.GetActiveRuntime();
	if (Runtime == nullptr)
	{
		SetEventRouterStateFailure(
			Scheduler,
			*Event.ExportName,
			OutResult);
		return false;
	}
	return Runtime->DispatchPreparedDelegateEvent(
		Event,
		NativeParameters,
		OutResult);
}
