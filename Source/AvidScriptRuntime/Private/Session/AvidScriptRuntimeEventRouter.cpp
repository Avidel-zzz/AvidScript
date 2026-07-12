#include "AvidScriptRuntimeEventRouter.h"

#include "AvidScriptRuntimeScheduler.h"

namespace
{
void SetEventRouterStateFailure(
	const FAvidScriptRuntimeScheduler& Scheduler,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = Scheduler.GetModuleId();
	OutResult.ExportName = TEXT("avid_on_event");
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
		SetEventRouterStateFailure(Scheduler, OutResult);
		return false;
	}

	return Runtime->DispatchEvent(EventId, Value, OutResult);
}
