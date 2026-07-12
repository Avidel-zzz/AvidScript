#pragma once

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

private:
	FAvidScriptRuntimeScheduler& Scheduler;
};
