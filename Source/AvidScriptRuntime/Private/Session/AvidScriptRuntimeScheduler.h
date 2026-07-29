#pragma once

#include "AvidScriptWasmRuntime.h"

class FAvidScriptRuntimeScheduler
{
public:
	void Attach(FAvidScriptWasmRuntimeInstance& Runtime);
	void Detach();

	bool Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool TickHot(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutFailure);
	bool Tick(
		float DeltaSeconds,
		FAvidScriptWasmSmokeResult& OutResult,
		EAvidScriptWasmResultDetail ResultDetail);
	bool IsAttached() const { return ActiveRuntime != nullptr; }
	bool IsAttachedTo(const FAvidScriptWasmRuntimeInstance* Runtime) const { return ActiveRuntime == Runtime; }
	FAvidScriptWasmRuntimeInstance* GetActiveRuntime() const { return ActiveRuntime; }
	EAvidScriptLifecycleState GetLifecycleState() const;
	FString GetModuleId() const;
	int32 GetTickCallCount() const;
	int32 GetPendingTimerCount() const;
	int32 GetTimerCallbackCount() const;
	int32 GetEventCallbackCount() const;

private:
	FAvidScriptWasmRuntimeInstance* ActiveRuntime = nullptr;
};
