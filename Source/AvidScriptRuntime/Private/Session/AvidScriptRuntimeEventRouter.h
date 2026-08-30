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
	bool DispatchHot(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutFailure);
	bool DispatchHot(
		const FAvidScriptGameplayEvent& Event,
		FAvidScriptWasmSmokeResult& OutFailure);
	bool Dispatch(
		const FAvidScriptPreparedDelegateEvent& Event,
		void* NativeParameters,
		FAvidScriptWasmSmokeResult& OutResult);

private:
	FAvidScriptRuntimeScheduler& Scheduler;
};
