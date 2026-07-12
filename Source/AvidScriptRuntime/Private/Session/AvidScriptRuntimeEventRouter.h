#pragma once

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptWasmRuntime.h"

class FAvidScriptRuntimeScheduler;

class FAvidScriptRuntimeEventRouter
{
public:
	explicit FAvidScriptRuntimeEventRouter(FAvidScriptRuntimeScheduler& InScheduler)
		: Scheduler(InScheduler)
	{
	}

	bool Dispatch(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
	bool Dispatch(const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult);

private:
	FAvidScriptRuntimeScheduler& Scheduler;
};
