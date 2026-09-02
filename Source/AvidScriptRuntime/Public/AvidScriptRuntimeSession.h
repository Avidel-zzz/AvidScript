#pragma once

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptDebug.h"
#include "AvidScriptWasmReloadTypes.h"
#include "Profiling/AvidScriptProfiler.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"

class FAvidScriptRuntimeEventRouter;
class FAvidScriptRuntimeScheduler;
class FAvidScriptSessionObjectOwnership;
class FAvidScriptSessionDelegateSubscriptions;
class FAvidScriptSessionInboundHandlers;
class FAvidScriptSessionContinuations;
class FAvidScriptSessionDebugger;
class IAvidScriptBindingHostEffectJournal;
class FAvidScriptGeneratedTypeRegistrySnapshot;
struct FAvidScriptGeneratedPreparedTypeRoute;
struct FAvidScriptRuntimeGeneratedTypeInstanceState;
struct FAvidScriptRuntimeArtifact;

struct AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeSessionSnapshot
{
	EAvidScriptLifecycleState LifecycleState = EAvidScriptLifecycleState::Empty;
	bool bHasActiveRuntime = false;
	bool bFaultQuarantined = false;
	FString ModuleId;
	FString FaultCategory;
	int32 TickCallCount = 0;
	int32 PendingTimerCount = 0;
	int32 PendingContinuationCount = 0;
	int32 TimerCallbackCount = 0;
	int32 EventCallbackCount = 0;
	int32 SuccessfulReloadCount = 0;
	int32 RejectedReloadCount = 0;
};

#if WITH_DEV_AUTOMATION_TESTS
struct AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeSessionTestSnapshot
{
	FAvidScriptRuntimeSessionSnapshot Runtime;
	FAvidScriptWasmReloadManifest LiveManifest;
	FAvidScriptWasmHostContext HostContext;
	const FAvidScriptWasmRuntimeInstance* LiveRuntimeIdentity = nullptr;
	bool bSchedulerAttached = false;
};
#endif

class AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeSession
	: public IAvidScriptGeneratedTypeInstance
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
	bool LoadInitialArtifact(
		const FAvidScriptRuntimeArtifact& Artifact,
		FAvidScriptWasmReloadResult& OutResult);
	bool ReloadArtifact(
		const FAvidScriptRuntimeArtifact& Artifact,
		FAvidScriptWasmReloadResult& OutResult);

	bool Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEvent(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchGameplayEvent(const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult);
	bool StopAndUnload(FAvidScriptWasmSmokeResult& OutResult);
	bool TickLive(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool TickHot(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutFailure);
	bool DispatchEventLive(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEventHot(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutFailure);
	bool DispatchGameplayEventLive(const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchGameplayEventHot(
		const FAvidScriptGameplayEvent& Event,
		FAvidScriptWasmSmokeResult& OutFailure);
	bool DispatchPreparedDelegateEvent(
		const FAvidScriptPreparedDelegateEvent& Event,
		void* NativeParameters,
		FAvidScriptWasmSmokeResult& OutResult);
	bool CaptureLiveSnapshot(FAvidScriptWasmSmokeResult& OutResult) const;
	bool EndPlayLive(FAvidScriptWasmSmokeResult& OutResult);
	void SetHostContext(const FAvidScriptWasmHostContext& InHostContext);
	void ClearHostContext();
	void UnloadLive();
	bool ConfigureGeneratedTypeInstance(
		UObject& Receiver,
		const FAvidScriptObjectHandle& ReceiverHandle,
		uint32 TypeOrdinal,
		const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
		FString& OutError);
	bool ClearGeneratedTypeInstance(FString& OutError);
	bool InvokeGeneratedTypeMember(
		UObject& Receiver,
		const FAvidScriptObjectHandle& ReceiverHandle,
		uint32 TypeOrdinal,
		uint32 MemberOrdinal,
		TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result) override;

	bool IsLiveLoaded() const;
	bool IsOperationActive() const { return bMutationInProgress || ActiveGuestCallDepth > 0; }
	bool AttachDebugger(TConstArrayView<uint64> BreakpointProbeIds);
	bool DetachDebugger();
	bool SetDebugBreakpoints(TConstArrayView<uint64> BreakpointProbeIds);
	bool RequestDebugPause();
	bool ContinueDebugExecution();
	bool ContinueDebugExecution(FAvidScriptWasmSmokeResult& OutResult);
	bool StepIntoDebugExecution();
	bool StepIntoDebugExecution(FAvidScriptWasmSmokeResult& OutResult);
	FAvidScriptDebugSessionSnapshot GetDebugSnapshot() const;
	bool GetDebugBreakpointCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const;
	bool GetDebugVariables(
		FAvidScriptDebugVariablesSnapshot& OutSnapshot,
		FString& OutError) const;
	void SetProfilerEnabled(bool bEnabled);
	bool IsProfilerEnabled() const;
	void ResetProfiler();
	FAvidScriptProfilerSnapshot GetProfilerSnapshot() const;
	FString GetLiveModuleId() const;
	int32 GetLiveTickCallCount() const;
	int32 GetLivePendingTimerCount() const;
	int32 GetLivePendingContinuationCount() const;
	int32 GetLiveTimerCallbackCount() const;
	int32 GetLiveEventCallbackCount() const;
	EAvidScriptLifecycleState GetLiveLifecycleState() const;
	FAvidScriptWasmHotSnapshot GetLiveHotSnapshot() const;
	FAvidScriptRuntimeSessionSnapshot GetSnapshot() const;
	int32 GetSuccessfulReloadCount() const { return SuccessfulReloadCount; }
	int32 GetRejectedReloadCount() const { return RejectedReloadCount; }
#if WITH_DEV_AUTOMATION_TESTS
	FAvidScriptWasmRuntimeInstance* GetLiveRuntimeForTesting() const { return LiveRuntime.Get(); }
	void SetBackendSelectionForTesting(const FAvidScriptVmBackendSelection& InBackendSelection)
	{
		check(!LiveRuntime);
		BackendSelection = InBackendSelection;
	}
	void SetCandidateBeginPlayObserverForTesting(TFunction<void()> InObserver)
	{
		CandidateBeginPlayObserverForTesting =
			[Observer = MoveTemp(InObserver)](IAvidScriptBindingHostEffectJournal*) mutable
			{
				Observer();
			};
	}
	void SetCandidateBeginPlayObserverForTesting(
		TFunction<void(IAvidScriptBindingHostEffectJournal*)> InObserver)
	{
		CandidateBeginPlayObserverForTesting = MoveTemp(InObserver);
	}
	void SetLiveExecutionObserverForTesting(TFunction<void()> InObserver)
	{
		LiveExecutionObserverForTesting = MoveTemp(InObserver);
	}
	FAvidScriptRuntimeSessionTestSnapshot GetTestSnapshot() const;
	bool PrepareDelegateSubscriptionsForTesting(
		UObject* Source,
		TConstArrayView<FAvidScriptPreparedDelegateEvent> Events,
		FString& OutError);
	void CommitDelegateSubscriptionsForTesting();
	void UnbindDelegateSubscriptionsForTesting();
	int32 GetDelegateSubscriptionCountForTesting() const;
	bool PrepareInboundHandlersForTesting(
		UObject* Source,
		TConstArrayView<FAvidScriptPreparedDelegateEvent> Handlers,
		FString& OutError);
	bool CommitInboundHandlersForTesting(FString& OutError);
	void UnbindInboundHandlersForTesting();
	int32 GetInboundHandlerCountForTesting() const;
	int32 GetDeferredInboundHandlerCountForTesting() const;
	int32 GetPreparedContinuationCountForTesting() const;
	int64 SubscribeDelegateForTesting(
		UObject& Source,
		uint32 EventOrdinal,
		FString& OutError);
	bool UnsubscribeDelegateForTesting(
		int64 SubscriptionToken,
		FString& OutError);
#endif

private:
	bool ValidateManifest(
		const FAvidScriptWasmReloadManifest& Manifest,
		const FString& PreviousModuleId,
		FAvidScriptWasmReloadResult& OutResult) const;

	bool BuildValidatedRuntime(
		const FAvidScriptRuntimeArtifact& Artifact,
		TUniquePtr<FAvidScriptWasmRuntimeInstance>& OutRuntime,
		FAvidScriptWasmReloadResult& OutResult) const;
	bool ValidateExpectedOwner(
		const FAvidScriptWasmReloadManifest& Manifest,
		FAvidScriptWasmReloadResult& OutResult) const;
	bool ActivateValidatedRuntime(
		TUniquePtr<FAvidScriptWasmRuntimeInstance>& CandidateRuntime,
		const FAvidScriptWasmReloadManifest& Manifest,
		bool bUseHostEffectTransaction,
		FAvidScriptWasmReloadResult& OutResult);
	bool PrepareGeneratedTypeExports(
		FAvidScriptWasmRuntimeInstance& Runtime,
		TArray<FAvidScriptGeneratedPreparedTypeRoute>& OutRoutes,
		FString& OutError) const;
	bool PumpReadyContinuations(FAvidScriptWasmSmokeResult& OutResult);
	bool CanEnterGuest(
		const FString& ExportName,
		FAvidScriptWasmSmokeResult& OutResult) const;
	void QuarantineFaultedRuntime(
		const FAvidScriptWasmSmokeResult& Failure);
	void ClearFaultQuarantine();
	bool IsDebugExecutionSuspended() const;
	bool ResumeDebugExecution(
		EAvidScriptDebugRunMode RunMode,
		FAvidScriptWasmSmokeResult& OutResult);

	TUniquePtr<FAvidScriptSessionObjectOwnership> ObjectOwnership;
	TUniquePtr<FAvidScriptSessionDelegateSubscriptions> DelegateSubscriptions;
	TUniquePtr<FAvidScriptSessionInboundHandlers> InboundHandlers;
	TSharedPtr<FAvidScriptSessionContinuations> Continuations;
	TUniquePtr<FAvidScriptProfilerEventBuffer> Profiler;
	TUniquePtr<FAvidScriptSessionDebugger> Debugger;
	TUniquePtr<FAvidScriptWasmRuntimeInstance> LiveRuntime;
	TUniquePtr<FAvidScriptRuntimeScheduler> Scheduler;
	TUniquePtr<FAvidScriptRuntimeEventRouter> EventRouter;
	TUniquePtr<FAvidScriptRuntimeGeneratedTypeInstanceState> GeneratedTypeInstance;
	FAvidScriptWasmReloadManifest LiveManifest;
	FAvidScriptWasmHostContext HostContext;
	FAvidScriptVmBackendSelection BackendSelection;
	int32 SuccessfulReloadCount = 0;
	int32 RejectedReloadCount = 0;
	int32 ActiveGuestCallDepth = 0;
	bool bMutationInProgress = false;
	bool bFaultQuarantined = false;
	FString FaultedModuleId;
	FString FaultCategory;
#if WITH_DEV_AUTOMATION_TESTS
	TFunction<void(IAvidScriptBindingHostEffectJournal*)>
		CandidateBeginPlayObserverForTesting;
	TFunction<void()> LiveExecutionObserverForTesting;
#endif
};

using FAvidScriptWasmReloadSession = FAvidScriptRuntimeSession;
