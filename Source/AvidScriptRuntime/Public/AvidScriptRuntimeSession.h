#pragma once

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptWasmReloadTypes.h"

class FAvidScriptRuntimeEventRouter;
class FAvidScriptRuntimeScheduler;

struct AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeSessionSnapshot
{
	EAvidScriptLifecycleState LifecycleState = EAvidScriptLifecycleState::Empty;
	bool bHasActiveRuntime = false;
	FString ModuleId;
	int32 TickCallCount = 0;
	int32 PendingTimerCount = 0;
	int32 TimerCallbackCount = 0;
	int32 EventCallbackCount = 0;
	int32 SuccessfulReloadCount = 0;
	int32 RejectedReloadCount = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeSession
{
public:
	FAvidScriptRuntimeSession();
	~FAvidScriptRuntimeSession();
	bool LoadEmbeddedSmoke(FAvidScriptWasmReloadResult& OutResult);

	bool LoadInitialModule(
		const uint8* Bytecode,
		int32 BytecodeSize,
		const FAvidScriptWasmReloadManifest& Manifest,
		FAvidScriptWasmReloadResult& OutResult);

	bool ReloadModule(
		const uint8* Bytecode,
		int32 BytecodeSize,
		const FAvidScriptWasmReloadManifest& Manifest,
		FAvidScriptWasmReloadResult& OutResult);

	bool Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEvent(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchGameplayEvent(const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult);
	bool StopAndUnload(FAvidScriptWasmSmokeResult& OutResult);
	bool TickLive(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEventLive(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchGameplayEventLive(const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult);
	bool EndPlayLive(FAvidScriptWasmSmokeResult& OutResult);
	void SetHostContext(const FAvidScriptWasmHostContext& InHostContext);
	void ClearHostContext();
	void UnloadLive();

	bool IsLiveLoaded() const;
	FString GetLiveModuleId() const;
	int32 GetLiveTickCallCount() const;
	int32 GetLivePendingTimerCount() const;
	int32 GetLiveTimerCallbackCount() const;
	int32 GetLiveEventCallbackCount() const;
	FAvidScriptRuntimeSessionSnapshot GetSnapshot() const;
	int32 GetSuccessfulReloadCount() const { return SuccessfulReloadCount; }
	int32 GetRejectedReloadCount() const { return RejectedReloadCount; }

private:
	bool ValidateManifest(
		const FAvidScriptWasmReloadManifest& Manifest,
		const FString& PreviousModuleId,
		FAvidScriptWasmReloadResult& OutResult) const;

	bool BuildValidatedRuntime(
		const uint8* Bytecode,
		int32 BytecodeSize,
		const FAvidScriptWasmReloadManifest& Manifest,
		TUniquePtr<FAvidScriptWasmRuntimeInstance>& OutRuntime,
		FAvidScriptWasmReloadResult& OutResult) const;
	bool ActivateValidatedRuntime(
		TUniquePtr<FAvidScriptWasmRuntimeInstance>& CandidateRuntime,
		const FAvidScriptWasmReloadManifest& Manifest,
		bool bUseHostEffectTransaction,
		FAvidScriptWasmReloadResult& OutResult);

	TUniquePtr<FAvidScriptWasmRuntimeInstance> LiveRuntime;
	TUniquePtr<FAvidScriptRuntimeScheduler> Scheduler;
	TUniquePtr<FAvidScriptRuntimeEventRouter> EventRouter;
	FAvidScriptWasmReloadManifest LiveManifest;
	FAvidScriptWasmHostContext HostContext;
	int32 SuccessfulReloadCount = 0;
	int32 RejectedReloadCount = 0;
};

using FAvidScriptWasmReloadSession = FAvidScriptRuntimeSession;
