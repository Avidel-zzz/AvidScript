#pragma once

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptDataBridgeTypes.h"
#include "AvidScriptContinuation.h"
#include "AvidScriptDebug.h"
#include "AvidScriptWasmDiagnostics.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptLifecycleState.h"
#include "AvidScriptVmArtifact.h"
#include "AvidScriptVmBackend.h"
#include "AvidScriptActorBinding.h"
#include "AvidScriptArrayValueHeap.h"
#include "AvidScriptCompositeValueHeap.h"
#include "AvidScriptObjectOwnership.h"
#include "AvidScriptUtf8ValueHeap.h"

#include "CoreMinimal.h"

class FAvidScriptWasmDebugMap;
class FAvidScriptProfilerEventBuffer;
class UWorld;
class IAvidScriptGeneratedTypeAuthority;
class FAvidScriptGeneratedTypeRegistrySnapshot;
struct FAvidScriptGeneratedTypeHostBindings;
struct FAvidScriptManagedRootTransfer;
namespace AvidScript::Managed { class FHeap; }

class AVIDSCRIPTRUNTIME_API IAvidScriptEventSubscriptionHost
{
public:
	virtual ~IAvidScriptEventSubscriptionHost() = default;

	virtual int64 Subscribe(
		UObject& Source,
		uint32 EventOrdinal,
		FString& OutError) = 0;
	virtual bool Unsubscribe(int64 SubscriptionToken, FString& OutError) = 0;
	virtual bool IsCurrentSource(const UObject& Source) const { return false; }
};

struct FAvidScriptWasmRuntimeMetrics
{
	double RuntimeInitMs = 0.0;
	double ModuleLoadMs = 0.0;
	double ModuleInstantiateMs = 0.0;
	double ExecEnvCreateMs = 0.0;
	double BeginPlayCallMs = 0.0;
	double EndPlayCallMs = 0.0;
	double TimerCallbackCallMs = 0.0;
	double EventCallbackCallMs = 0.0;
	double HostImportCallMs = 0.0;
	int32 TimedDynamicHostCallCount = 0;
	double TickCallMs = 0.0;
	double UnloadMs = 0.0;
};

enum class EAvidScriptDynamicHostCallTimingPolicy : uint8
{
	Disabled,
	PerCall
};

enum class EAvidScriptWasmResultDetail : uint8
{
	FailureOnly,
	FullSnapshot,
	HotFailureOnly
};

struct FAvidScriptWasmSmokeResult
{
	bool bRuntimeInitialized = false;
	bool bModuleLoaded = false;
	bool bModuleInstantiated = false;
	bool bBeginPlayCalled = false;
	bool bEndPlayCalled = false;
	bool bTickCalled = false;
	bool bTimerCallbackCalled = false;
	bool bEventCallbackCalled = false;
	bool bUnloaded = false;
	int32 TickCallCount = 0;
	int32 TimerCallbackCount = 0;
	int32 LastTimerCallbackId = 0;
	int32 LastTimerHandle = 0;
	int32 EventCallbackCount = 0;
	int32 LastEventId = 0;
	float LastEventValue = 0.0f;
	FString ModuleId;
	FString ExportName;
	FString ImportModuleName;
	FString ImportName;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
	TArray<FAvidScriptWasmDiagnosticFrame> DiagnosticFrames;
	int32 HostImportCallCount = 0;
	int32 LastHostImportInput = 0;
	int32 LastHostImportResult = 0;
	FAvidScriptVmBackendInfo BackendInfo;
	FAvidScriptWasmRuntimeMetrics Metrics;
	FAvidScriptDataBridgeMetrics DataBridgeMetrics;
	FAvidScriptBindingInvocationInstrumentation BindingInstrumentation;
};

struct FAvidScriptWasmHotSnapshot
{
	bool bRuntimeLoaded = false;
	bool bBeginPlayCalled = false;
	bool bEndPlayCalled = false;
	int32 TickCallCount = 0;
	int32 TimerCallbackCount = 0;
	int32 LastTimerCallbackId = 0;
	int32 LastTimerHandle = 0;
	int32 EventCallbackCount = 0;
	int32 LastEventId = 0;
	float LastEventValue = 0.0f;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

class FAvidScriptWasmInstanceExecutionState;

struct FAvidScriptWasmHostContext
{
	TWeakPtr<IAvidScriptGeneratedTypeAuthority> GeneratedTypeAuthority;
	TSharedPtr<FAvidScriptWasmInstanceExecutionState> InstanceExecutionState;
	FAvidScriptObjectRegistry* ObjectRegistry = nullptr;
	IAvidScriptObjectOwnershipDomain* ObjectOwnership = nullptr;
	FAvidScriptObjectHandle OwnerHandle;
	TWeakObjectPtr<UWorld> World;
	EAvidScriptActorWritePolicy ActorWritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
	IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr;
	IAvidScriptEventSubscriptionHost* EventSubscriptions = nullptr;
	IAvidScriptContinuationHost* Continuations = nullptr;
	IAvidScriptDebugProbeHost* DebugProbes = nullptr;
	FAvidScriptProfilerEventBuffer* Profiler = nullptr;
	IAvidScriptBindingLatentHost* LatentHost = nullptr;
	EAvidScriptBindingInvocationPolicy BindingInvocationPolicy =
		EAvidScriptBindingInvocationPolicy::SemanticProcessEvent;
	FAvidScriptBindingInvocationInstrumentation* BindingInvocationInstrumentation = nullptr;
	EAvidScriptDynamicHostCallTimingPolicy DynamicHostCallTimingPolicy =
		EAvidScriptDynamicHostCallTimingPolicy::Disabled;
};

struct FAvidScriptWasmTimerEntry
{
	int32 Handle = 0;
	int32 CallbackId = 0;
	double DueTimeSeconds = 0.0;
};

struct FAvidScriptPreparedGeneratedHostCall;
struct FAvidScriptPreparedReflectionHostCall;
struct FAvidScriptPreparedDynamicHostCall;
struct FAvidScriptContinuationResultCodecTransaction;

struct FAvidScriptSelfCapability
{
	TWeakObjectPtr<UObject> Object;
	FAvidScriptObjectHandle Handle;
	uint64 ReloadEpoch = 0;
	uint64 CallbackEpoch = 0;
	uint64 RegistryRevision = 0;
};

struct FAvidScriptFusedCallbackFrame
{
	UObject* Receiver = nullptr;
	FAvidScriptObjectHandle Handle;
	uint64 CallbackEpoch = 0;
	uint64 ReloadEpoch = 0;
	uint64 RegistryRevision = 0;
	const void* PreparedReflectionGuardIdentity = nullptr;
	bool bPreparedReflectionNativeGuardAllowed = false;
};

struct FAvidScriptCachedVmExport
{
	FAvidScriptVmExportHandle Handle;
	FAvidScriptVmPreparedExportCall PreparedCall;
};

// A prepared export belongs to one loaded Runtime code generation. Native callers
// supply the validated instance context; this does not authorize Guest routing.
struct FAvidScriptContextualExportCall
{
	bool IsValid() const { return CodeIdentity.IsValid() && Call.IsValid(); }
	uint32 GetParameterCellCount() const { return Call.ParameterCellCount; }
	uint32 GetResultCellCount() const { return Call.ResultCellCount; }
	const FString& GetExportName() const { return ExportName; }
private:
	friend class FAvidScriptWasmRuntimeInstance;
	TSharedPtr<const uint8> CodeIdentity;
	FAvidScriptVmPreparedExportCall Call;
	FString ExportName;
};

// Per-owner execution state. VM memory, code and the managed heap remain Runtime-owned.
// Identity and mutation are private: native callers obtain states from the Runtime.
class AVIDSCRIPTRUNTIME_API FAvidScriptWasmInstanceExecutionState
{
public:
    ~FAvidScriptWasmInstanceExecutionState() = default;
    EAvidScriptLifecycleState GetLifecycleState() const { return LifecycleState.GetState(); }
    int32 GetPendingTimerCount() const { return ActiveTimers.Num(); }
    int32 GetTickCallCount() const { return TickCallCount; }
    int32 GetTimerCallbackCount() const { return TimerCallbackCount; }
    int32 GetEventCallbackCount() const { return EventCallbackCount; }
    bool IsRetired() const { return bRetired; }
private:
    friend class FAvidScriptWasmRuntimeInstance;
    FAvidScriptWasmInstanceExecutionState() = default;
    FAvidScriptWasmInstanceExecutionState(const FAvidScriptWasmInstanceExecutionState&) = delete;
    FAvidScriptWasmInstanceExecutionState& operator=(const FAvidScriptWasmInstanceExecutionState&) = delete;
    TSharedPtr<const uint8> CodeIdentity;
    FAvidScriptContextualExportCall LifecycleScopeCall;
    FAvidScriptObjectRegistry* ObjectRegistry = nullptr;
    FAvidScriptObjectHandle OwnerHandle;
    TWeakObjectPtr<UWorld> World;
    uint32 ActiveCalls = 0;
    bool bRetired = false;
	bool bHasBegunPlay = false;
	bool bHasEndedPlay = false;
	bool bEndPlayAttempted = false;
	bool bEndPlaySucceeded = false;
	int32 TickCallCount = 0;
	int32 NextTimerHandle = 1;
	int32 TimerCallbackCount = 0;
	int32 LastTimerCallbackId = 0;
	int32 LastTimerHandle = 0;
	TMap<int32, FAvidScriptWasmTimerEntry> ActiveTimers;
	TArray<FAvidScriptWasmTimerEntry> TimerHeap;
	TArray<FAvidScriptWasmTimerEntry> DueTimerScratch;
	double TimerClockSeconds = 0.0;
	int32 StaleTimerHeapEntryCount = 0;
	int32 EventCallbackCount = 0;
	int32 LastEventId = 0;
	float LastEventValue = 0.0f;
	int32 HostImportCallCount = 0;
	int32 LastHostImportInput = 0;
	int32 LastHostImportResult = 0;
	FAvidScriptWasmSmokeResult CachedEndPlayResult;
	FAvidScriptLifecycleStateMachine LifecycleState;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntimeInstance
	: public IAvidScriptHostDispatcher
	, public IAvidScriptVmTypedHostDispatcher
	, public IAvidScriptVmInvocationObserver
{
public:
	FAvidScriptWasmRuntimeInstance();
	explicit FAvidScriptWasmRuntimeInstance(const FAvidScriptVmBackendSelection& InBackendSelection);
	~FAvidScriptWasmRuntimeInstance();

	FAvidScriptWasmRuntimeInstance(const FAvidScriptWasmRuntimeInstance&) = delete;
	FAvidScriptWasmRuntimeInstance& operator=(const FAvidScriptWasmRuntimeInstance&) = delete;

	bool LoadEmbeddedSmokeModule(FAvidScriptWasmSmokeResult& OutResult);
	bool LoadEmbeddedHostImportModule(FAvidScriptWasmSmokeResult& OutResult);
	bool LoadModule(const uint8* Bytecode, int32 BytecodeSize, const FString& InModuleId, FAvidScriptWasmSmokeResult& OutResult);
	bool LoadModule(
		const uint8* Bytecode,
		int32 BytecodeSize,
		const FString& InModuleId,
		const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage,
		FAvidScriptWasmSmokeResult& OutResult);
	bool LoadModule(
		const uint8* Bytecode,
		int32 BytecodeSize,
		const FString& InModuleId,
		const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage,
		const TSharedPtr<const FAvidScriptWasmDebugMap>& InDebugMap,
		FAvidScriptWasmSmokeResult& OutResult);
	bool LoadArtifact(
		const FAvidScriptVmOwnedArtifact& Artifact,
		const FString& InModuleId,
		const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage,
		const TSharedPtr<const FAvidScriptWasmDebugMap>& InDebugMap,
		EAvidScriptVmArtifactTrust ArtifactTrust,
		FAvidScriptWasmSmokeResult& OutResult);
	bool ValidateRequiredExports(
		const TArray<FString>& RequiredExports,
		FAvidScriptWasmSmokeResult& OutResult);
	bool PrepareNamedExportCall(
		const FString& ExportName,
		FAvidScriptVmPreparedExportCall& OutCall,
		FString& OutError);
	bool PrepareContextualExportCall(
		const FString& ExportName, FAvidScriptContextualExportCall& OutCall, FString& OutError);
	// Call after ValidateRequiredExports; returns an empty call when no continuation
	// export was validated. The prepared entry follows that validation's V2/V1 choice.
	bool PrepareContextualContinuationCall(FAvidScriptContextualExportCall& OutCall, FString& OutError);
	// Create an owner-bound state after validating module exports. These lifecycle
	// calls affect only that state; they never unload the shared VM or heap.
	bool CreateInstanceExecutionState(const FAvidScriptWasmHostContext& Context,
		TSharedPtr<FAvidScriptWasmInstanceExecutionState>& OutState, FString& OutError);
	bool BeginPlayInContext(const FAvidScriptWasmHostContext& Context, FAvidScriptWasmSmokeResult& OutResult);
	bool TickInContext(const FAvidScriptWasmHostContext& Context, float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult,
		EAvidScriptWasmResultDetail ResultDetail = EAvidScriptWasmResultDetail::FullSnapshot);
	bool EndPlayInContext(const FAvidScriptWasmHostContext& Context, FAvidScriptWasmSmokeResult& OutResult);
	bool RetireInstanceExecutionState(const TSharedPtr<FAvidScriptWasmInstanceExecutionState>& State, FString& OutError);
	bool DispatchEventInContext(const FAvidScriptWasmHostContext& Context, int32 EventId, float Value,
		FAvidScriptWasmSmokeResult& OutResult, EAvidScriptWasmResultDetail Detail = EAvidScriptWasmResultDetail::FullSnapshot);
	bool DispatchGameplayEventInContext(const FAvidScriptWasmHostContext& Context, const FAvidScriptGameplayEvent& Event,
		FAvidScriptWasmSmokeResult& OutResult, EAvidScriptWasmResultDetail Detail = EAvidScriptWasmResultDetail::FullSnapshot);
	bool DispatchDebugResumeInContext(const FAvidScriptWasmHostContext& Context, int64 Token, uint32 Route, FAvidScriptWasmSmokeResult& OutResult);
	bool CaptureSnapshotInContext(const FAvidScriptWasmHostContext& Context, FAvidScriptWasmSmokeResult& OutResult);
	FAvidScriptWasmHotSnapshot GetHotSnapshotInContext(const FAvidScriptWasmHostContext& Context);
	void RecordContextualFailure(const FAvidScriptWasmHostContext& Context, const FString& ExportName,
		const FAvidScriptVmError& Error, FAvidScriptWasmSmokeResult& OutResult);
	bool InvokeInContext(
		const FAvidScriptContextualExportCall& Call, const FAvidScriptWasmHostContext& Context,
		const FAvidScriptVmCallFrame& Frame, FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult = nullptr);
	// Native routing primitive for an active generated instance. The prepared
	// code and all pointers/tokens must belong to this same execution domain.
	// Language method selection and signature marshalling happen before this call.
	bool InvokeGeneratedInstanceExport(const FAvidScriptObjectHandle& Target,
		const FAvidScriptContextualExportCall& Call, const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError, FAvidScriptVmCallResult* OutResult = nullptr,
		TConstArrayView<uint64> ReturnedRoots = {});
	bool SetSupplementalTypedHostImports(
		TConstArrayView<FAvidScriptVmTypedHostImport> Imports,
		FString& OutError);
	bool ConfigureGeneratedTypeHostBindings(
		const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
		TArray<FAvidScriptVmTypedHostImport>& OutImports, FString& OutError,
		TConstArrayView<uint8> CanonicalWasm = {});
	bool PrepareGeneratedMethodHostBindings(FString& OutError);
	bool InvokeGeneratedMethodFrame(uint32 RouteIndex, int32 FrameAddress, int32 FrameBytes);
	bool ResolveGeneratedReceiverType(int64 PackedSelf,
		const FAvidScriptGeneratedTypeRegistrySnapshot& Registry, uint32& OutOrdinal);
	UObject* ResolveGeneratedTypeReceiver(
		int64 PackedSelf, uint32 TypeOrdinal, const FAvidScriptGeneratedTypeRegistrySnapshot& Registry) const;
	bool ConfigureExecutionBudget(
		const FAvidScriptVmLoadConfig::FExecutionBudget& InBudget,
		FString& OutError);
	void RecordPreparedVmFailure(
		const FString& ExportName,
		const FAvidScriptVmError& Error,
		FAvidScriptWasmSmokeResult& OutResult);
	bool BeginPlay(FAvidScriptWasmSmokeResult& OutResult);
	bool Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool Tick(
		float DeltaSeconds,
		FAvidScriptWasmSmokeResult& OutResult,
		EAvidScriptWasmResultDetail ResultDetail);
	bool TickHot(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutFailure);
	bool EndPlay(FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEvent(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEventHot(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutFailure);
	bool DispatchGameplayEvent(const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchContinuation(
		const FAvidScriptContinuationCompletion& Completion,
		FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchContinuationInContext(
		const FAvidScriptContextualExportCall& Call, const FAvidScriptWasmHostContext& Context,
		const FAvidScriptContinuationCompletion& Completion, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchDebugResume(
		int64 SuspensionToken,
		uint32 ResumeRoute,
		FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchGameplayEventHot(
		const FAvidScriptGameplayEvent& Event,
		FAvidScriptWasmSmokeResult& OutFailure);
	bool BuildPreparedDelegateEvents(
		TArray<FAvidScriptPreparedDelegateEvent>& OutEvents,
		FString& OutError);
	bool BuildPreparedCallbacks(
		TArray<FAvidScriptPreparedDelegateEvent>& OutDelegateEvents,
		TArray<FAvidScriptPreparedDelegateEvent>& OutInboundHandlers,
		FString& OutError);
	bool DispatchPreparedDelegateEvent(
		const FAvidScriptPreparedDelegateEvent& Event,
		void* NativeParameters,
		FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchPreparedDelegateEventInContext(
		const FAvidScriptContextualExportCall& Call, const FAvidScriptWasmHostContext& Context,
		const FAvidScriptPreparedDelegateEvent& Event, void* NativeParameters,
		FAvidScriptWasmSmokeResult& OutResult);
	void CaptureSnapshot(FAvidScriptWasmSmokeResult& OutResult) const;
	FAvidScriptWasmHotSnapshot GetHotSnapshot() const;
	void Unload();
	void Unload(FAvidScriptWasmSmokeResult& OutResult);
	bool ReadStateBytes(uint32 GuestAddress, TArrayView<uint8> OutBytes, FString& OutError) const;
	bool WriteStateBytes(uint32 GuestAddress, TConstArrayView<uint8> Bytes, FString& OutError);
#if WITH_DEV_AUTOMATION_TESTS
	AvidScript::Managed::FHeap* GetManagedHeapForTesting() const { return ManagedHeap.Get(); }
	bool InvokeI32PairExportHotForTesting(
		const FString& ExportName,
		int32 FirstArgument,
		int32 SecondArgument,
		FAvidScriptWasmSmokeResult& OutFailure);
	void SetStateWriteFailuresForTesting(TConstArrayView<int32> InWriteAttempts);
	void ClearStateWriteFailureForTesting();
	bool PrepareDelegateEventExportsForTesting(
		TArray<FAvidScriptPreparedDelegateEvent>& InOutEvents,
		FString& OutError)
	{
		return PrepareDelegateEventExports(InOutEvents, OutError);
	}
	void BeginTypedCallbackEpochForTesting() { BeginTypedCallbackEpoch(); }
	void EndTypedCallbackEpochForTesting() { EndTypedCallbackEpoch(); }
	void SetBindingPackageForTesting(
		const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage)
	{
		BindingPackage = InBindingPackage;
	}
	bool BuildPreparedTypedHostImportsForTesting(FString& OutError)
	{
		if (BindingPackage.IsValid())
		{
			BindingInvocationScratch.SetNumUninitialized(
				BindingPackage->GetRequiredScratchSize());
		}
		else
		{
			BindingInvocationScratch.Reset();
		}
		return BuildPreparedTypedHostImports(OutError);
	}
	const TArray<FAvidScriptVmTypedHostImport>&
	GetPreparedTypedHostImportsForTesting() const
	{
		return TypedHostImports;
	}
	const FAvidScriptVmBindingPackage&
	GetPreparedVmBindingPackageForTesting() const
	{
		return PreparedVmBindingPackage;
	}
	int32 GetPreparedDynamicHostCallCountForTesting() const
	{
		return PreparedDynamicHostCalls.Num();
	}
	uint64 GetActiveCallbackEpochForTesting() const
	{
		return FusedCallbackFrameStack.IsEmpty()
			? 0
			: FusedCallbackFrameStack.Last().CallbackEpoch;
	}
	bool ResolveSelfCapabilityForTesting(
		int32 SelfSlot,
		int32 SelfGeneration,
		UClass* ExpectedClass,
		UObject*& OutObject)
	{
		return ResolveSelfCapability(
			SelfSlot,
			SelfGeneration,
			ExpectedClass,
			OutObject);
	}
	uint64 GetReloadEpochForTesting() const { return ReloadEpoch; }
	int32 GetHostImportCallCountForTesting() const { return GetInstanceState().HostImportCallCount; }
	FAvidScriptUtf8ValueHeap& GetUtf8ValueHeapForTesting()
	{
		return Utf8ValueHeap;
	}
	FAvidScriptArrayValueHeap& GetArrayValueHeapForTesting()
	{
		return ArrayValueHeap;
	}
	FAvidScriptCompositeValueHeap& GetCompositeValueHeapForTesting()
	{
		return CompositeValueHeap;
	}
	const FAvidScriptBindingInvocationContext&
	GetBindingInvocationContextForTesting() const
	{
		return BindingInvocationContext;
	}
	EAvidScriptVmTypedHostStatus RecordGeneratedStatusForTesting(
		EAvidScriptVmTypedHostStatus Status)
	{
		return RecordGeneratedStatus(Status);
	}
	const FAvidScriptVmLoadConfig::FExecutionBudget&
	GetExecutionBudgetForTesting() const
	{
		return ExecutionBudget;
	}
#endif

	bool IsLoaded() const;
	// Game-thread query for owners that must defer mutation until contextual calls unwind.
	bool IsContextInvocationActive() const { return ContextInvocationDepth != 0; }
	const FAvidScriptVmBackendInfo& GetActiveBackendInfo() const
	{
		return ActiveBackendInfo;
	}
	EAvidScriptLifecycleState GetLifecycleState() const { return GetInstanceState().LifecycleState.GetState(); }
	bool HasBegunPlay() const { return GetInstanceState().bHasBegunPlay; }
	int32 GetTickCallCount() const { return GetInstanceState().TickCallCount; }
	int32 GetPendingTimerCount() const { return GetInstanceState().ActiveTimers.Num(); }
	int32 GetTimerCallbackCount() const { return GetInstanceState().TimerCallbackCount; }
	int32 GetEventCallbackCount() const { return GetInstanceState().EventCallbackCount; }
	const FString& GetModuleId() const { return ModuleId; }
	const FAvidScriptWasmRuntimeMetrics& GetMetrics() const { return Metrics; }
	const FAvidScriptDataBridgeMetrics& GetDataBridgeMetrics() const { return DataBridgeMetrics; }
	void SetHostContext(const FAvidScriptWasmHostContext& InHostContext);
	void ClearHostContext();
	int32 HandleHostAddI32Import(int32 Input);
	int32 HandleHostFailI32Import(int32 Input);
	int32 HandleOwnerGetSlotImport();
	int32 HandleOwnerGetGenerationImport();
	int64 HandleOwnerGetHandleImport();
	int64 HandleDataLaneGetEpochImport();
	int32 HandleDataLaneSubmitImport(TConstArrayView<uint8> Bytes);
	int32 HandleValueArrayLengthImport(int32 Token);
	int32 HandleValueArrayLoadImport(int32 Token, int32 ElementIndex, TArrayView<uint8> OutBytes);
	int32 HandleValueArrayStoreImport(int32 Token, int32 ElementIndex, TConstArrayView<uint8> Bytes);
	int32 HandleValueArrayRangeImport(
		bool bReadFromCapability,
		int32 Token,
		int32 CapabilityIndex,
		uint32 GuestArrayReference,
		int32 GuestIndex,
		int32 ElementCount);
	int32 HandleValueReleaseImport(int32 Token);
	int32 HandleValueTextToStringImport(int32 Token);
	int32 HandleValueContainerCountImport(int32 Token);
	int32 HandleValueContainerAccessImport(
		bool bRead,
		int32 Token,
		int32 Index,
		int32 Lane,
		uint32 GuestAddress);
	int32 HandleValueContainerResizeImport(int32 Token, int32 NewCount);
	int32 HandleValueContainerClearImport(int32 Token);
	int32 HandleValueContainerFindImport(int32 Token, uint32 GuestAddress);
	int32 HandleValueContainerUpsertImport(
		int32 Token,
		uint32 KeyAddress,
		uint32 ValueAddress);
	int32 HandleValueContainerRemoveImport(int32 Token, uint32 KeyAddress);
	int32 HandleTimerSetOnceImport(float DelaySeconds, int32 CallbackId);
	int32 HandleTimerCancelImport(int32 TimerHandle);
	int64 HandleContinuationDelayImport(float DelaySeconds, int32 CallbackId);
	int64 HandleContinuationLoadObjectImport(
		int32 Utf8ValueReference,
		int32 CallbackId);
	int32 HandleContinuationCancelImport(int64 ContinuationToken);
	int64 HandleContinuationCancelSourceCreateImport();
	int32 HandleContinuationCancelSourceCancelImport(int64 SourceToken);
	int32 HandleContinuationCancelSourceReleaseImport(int64 SourceToken);
	int32 HandleContinuationBindCancelImport(
		int64 SourceToken,
		int64 ContinuationToken);
	int32 HandleContinuationStateStoreImport(
		int64 ContinuationToken,
		TConstArrayView<uint8> StateBytes);
	int32 HandleContinuationStateReadImport(
		int64 ContinuationToken,
		TArrayView<uint8> OutStateBytes);
	int32 HandleContinuationResultReadImport(
		int32 BindingOrdinal,
		int32 ResultSlot,
		int32 ResultGeneration,
		TArrayView<uint8> OutBytes);
	int64 HandleEventSubscribeImport(int32 Slot, int32 Generation, int32 EventOrdinal);
	int32 HandleEventUnsubscribeImport(int64 SubscriptionToken);
	int32 HandleEventIsCurrentSourceImport(int32 Slot, int32 Generation);
	int32 HandleDelegateOutputWriteImport(
		int32 TransactionToken,
		int32 OutputOrdinal,
		uint32 GuestAddress);
	int32 HandleActorGetLocationImport(int32 Slot, int32 Generation, FVector& OutLocation);
	int32 HandleActorSetLocationImport(int32 Slot, int32 Generation, const FVector& Location);
	int32 HandleActorAddLocationOffsetImport(int32 Slot, int32 Generation, const FVector& Offset);
	int32 HandleActorGetRotationImport(int32 Slot, int32 Generation, FRotator& OutRotation);
	int32 HandleActorSetRotationImport(int32 Slot, int32 Generation, const FRotator& Rotation);
	int32 HandleActorGetScaleImport(int32 Slot, int32 Generation, FVector& OutScale3D);
	int32 HandleActorSetScaleImport(int32 Slot, int32 Generation, const FVector& Scale3D);
	bool HandleActorGetTransformBatchImport(
		int32 RequestedCount,
		TConstArrayView<uint32> InputCells,
		TArrayView<float> OutputFloats,
		int32& OutProcessedCount);
	int32 HandleActorGetRootComponentImport(int32 Slot, int32 Generation, FAvidScriptObjectHandle& OutComponentHandle);
	int32 HandleSceneComponentGetWorldLocationImport(int32 Slot, int32 Generation, FVector& OutWorldLocation);
	int32 HandleSceneComponentSetWorldLocationImport(int32 Slot, int32 Generation, const FVector& WorldLocation);
	void SetPendingHostImportFailure(
		const FString& ImportModuleName,
		const FString& ImportName,
		const FString& Details,
		const TCHAR* ErrorCategory = TEXT("host_import_failed"));
	bool ConsumePendingHostImportFailure(
		FString& OutImportModuleName,
		FString& OutImportName,
		FString& OutDetails,
		FString* OutErrorCategory = nullptr);
	bool ConsumeTypedHostFailure(
		const FString& ExpectedModuleName,
		const FString& ExpectedImportName,
		FAvidScriptVmTypedHostFailure& OutFailure) override;
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) override;
	uint64 BeginVmInvocation() override;
	void EndVmInvocation(uint64 Token) override;
	bool DispatchManagedHeapCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult);
	bool DispatchContinuationManagedStateCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult);
	// Native callers retain references extracted from a validated state layout.
	// Failure leaves OutLease unchanged; use/release on the Game Thread.
	bool CreateContinuationStateLease(TConstArrayView<uint64> Objects,
		TUniquePtr<IAvidScriptContinuationStateLease>& OutLease);
	bool DispatchDynamicHostCall(
		const FAvidScriptDynamicHostCall& Call,
		FAvidScriptDynamicHostCallResult& OutResult) override;
	EAvidScriptVmTypedHostStatus DispatchEmptyI32(
		uint32 BindingOrdinal,
		int32& OutValue) override;
	EAvidScriptVmTypedHostStatus DispatchI32PairToI32(
		uint32 BindingOrdinal,
		int32 Left,
		int32 Right,
		int32& OutValue) override;
	EAvidScriptVmTypedHostStatus DispatchSelfI32PairToI32(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue) override;
	EAvidScriptVmTypedHostStatus DispatchSelfPropertyI32GetSet(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue) override;
	EAvidScriptVmTypedHostStatus DispatchSelfVectorValue(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue) override;
	EAvidScriptVmTypedHostStatus DispatchSelfVectorRefOut(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue) override;
	EAvidScriptVmTypedHostStatus DispatchStableObjectRoundtrip(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 ObjectSlot,
		int32 ObjectGeneration,
		int32 GuestAddress,
		int32& OutValue) override;
	EAvidScriptVmTypedHostStatus DispatchCommandBufferSubmit(
		uint32 BindingOrdinal,
		int32 GuestAddress,
		int32 ByteCount,
		int32& OutValue) override;


private:
	enum class EInstanceLifecycleOperation : uint8 { Begin, Tick, End };
	friend class FAvidScriptManagedContinuationStateLease;
	bool BeginPlayInternal(FAvidScriptWasmSmokeResult& OutResult);
	bool TickInternal(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult, EAvidScriptWasmResultDetail ResultDetail);
	bool EndPlayInternal(FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEventInternal(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult, EAvidScriptWasmResultDetail Detail);
	bool DispatchGameplayEventInternal(const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult, EAvidScriptWasmResultDetail Detail);
	bool DispatchDebugResumeInternal(int64 Token, uint32 Route, FAvidScriptWasmSmokeResult& OutResult);
	bool InvokeInstanceCallback(const FAvidScriptWasmHostContext& Context, const FString& ExportName,
		TFunctionRef<bool()> Operation, FAvidScriptWasmSmokeResult& OutResult, bool bResumeSuspended = false);
	bool ValidateInstanceExecutionState(const FAvidScriptWasmHostContext& Context) const;
	bool InvokeInstanceLifecycle(const FAvidScriptWasmHostContext& Context, EInstanceLifecycleOperation Operation,
		float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult,
		EAvidScriptWasmResultDetail ResultDetail = EAvidScriptWasmResultDetail::FullSnapshot);
	bool InvokeContextOperation(
		const FAvidScriptContextualExportCall& Call, const FAvidScriptWasmHostContext& Context,
		TFunctionRef<bool(FAvidScriptVmError&)> Operation, FAvidScriptVmError& OutError, bool bResumeSuspended = false);
	bool DispatchContinuationInternal(
		const FAvidScriptContinuationCompletion& Completion, FAvidScriptWasmSmokeResult& OutResult,
		const FAvidScriptVmPreparedExportCall* ScopedCall);
	bool DispatchPreparedDelegateEventInternal(
		const FAvidScriptPreparedDelegateEvent& Event, void* NativeParameters,
		FAvidScriptWasmSmokeResult& OutResult, const FAvidScriptVmPreparedExportCall* ScopedCall);
	bool ValidateContextCallbackCommit(FAvidScriptVmError& OutError) const;
	void ApplyHostContext(const FAvidScriptWasmHostContext& Context);
	bool RejectActiveContextMutation(const TCHAR* Operation, FAvidScriptWasmSmokeResult* OutResult = nullptr);
	bool ValidateInvocationContext(const FAvidScriptWasmHostContext& Context, bool bAllowSuspended = false) const;
	void LatchContextInvocationFailure(const FAvidScriptVmError& Error);
	bool LoadArtifactView(
		const FAvidScriptVmArtifactView& Artifact,
		const FString& InModuleId,
		const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage,
		const TSharedPtr<const FAvidScriptWasmDebugMap>& InDebugMap,
		FAvidScriptWasmSmokeResult& OutResult);
	bool BuildPreparedTypedHostImports(FString& OutError);
	bool BuildPreparedDynamicHostImports(FString& OutError);
	static bool InvokePreparedDynamicHost(
		void* Context,
		TConstArrayView<uint64> Arguments,
		IAvidScriptVmGuestMemory& GuestMemory,
		FAvidScriptDynamicHostCallResult& OutResult);
	bool DispatchPreparedDynamicHost(
		FAvidScriptPreparedDynamicHostCall& Call,
		TConstArrayView<uint64> Arguments,
		IAvidScriptVmGuestMemory& GuestMemory,
		FAvidScriptDynamicHostCallResult& OutResult);
	static EAvidScriptVmTypedHostStatus InvokePreparedSelfI32(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value,
		int32& OutValue);
	static int32 InvokePreparedDirectSelfI32(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value,
		int32* OutValue);
	EAvidScriptVmTypedHostStatus DispatchPreparedSelfI32(
		FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value,
		int32& OutValue);
	static EAvidScriptVmTypedHostStatus InvokePreparedSelfI32Pair(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue);
	EAvidScriptVmTypedHostStatus DispatchPreparedSelfI32Pair(
		FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue);
	static EAvidScriptVmTypedHostStatus
		InvokePreparedReflectionSelfI32GuestResult(
			void* Context,
			int32 SelfSlot,
			int32 SelfGeneration,
			int32 Value,
			int32 GuestAddress,
			int32& OutStatus);
	EAvidScriptVmTypedHostStatus
		DispatchPreparedReflectionSelfI32GuestResult(
			FAvidScriptPreparedReflectionHostCall& Call,
			int32 SelfSlot,
			int32 SelfGeneration,
			int32 Value,
			int32 GuestAddress,
			int32& OutStatus);
	static EAvidScriptVmTypedHostStatus
		InvokePreparedReflectionSelfI32PairGuestResult(
			void* Context,
			int32 SelfSlot,
			int32 SelfGeneration,
			int32 Left,
			int32 Right,
			int32 GuestAddress,
			int32& OutStatus);
	EAvidScriptVmTypedHostStatus
		DispatchPreparedReflectionSelfI32PairGuestResult(
			FAvidScriptPreparedReflectionHostCall& Call,
			int32 SelfSlot,
			int32 SelfGeneration,
			int32 Left,
			int32 Right,
			int32 GuestAddress,
			int32& OutStatus);
	static EAvidScriptVmTypedHostStatus
		InvokePreparedReflectionSelfGuestAddress(
			void* Context,
			int32 SelfSlot,
			int32 SelfGeneration,
			int32 GuestAddressOrValue,
			int32& OutStatus);
	EAvidScriptVmTypedHostStatus
		DispatchPreparedReflectionSelfGuestAddress(
			FAvidScriptPreparedReflectionHostCall& Call,
			int32 SelfSlot,
			int32 SelfGeneration,
			int32 GuestAddressOrValue,
			int32& OutStatus);
	static EAvidScriptVmTypedHostStatus
		InvokePreparedReflectionSelfF32TripleGuestVector(
			void* Context,
			int32 SelfSlot,
			int32 SelfGeneration,
			float X,
			float Y,
			float Z,
			int32 GuestAddress,
			int32& OutStatus);
	EAvidScriptVmTypedHostStatus
		DispatchPreparedReflectionSelfF32TripleGuestVector(
			FAvidScriptPreparedReflectionHostCall& Call,
			int32 SelfSlot,
			int32 SelfGeneration,
			float X,
			float Y,
			float Z,
			int32 GuestAddress,
			int32& OutStatus);
	static EAvidScriptVmTypedHostStatus
		InvokePreparedReflectionStableObjectRoundtrip(
			void* Context,
			int32 SelfSlot,
			int32 SelfGeneration,
			int32 ObjectSlot,
			int32 ObjectGeneration,
			int32 GuestAddress,
			int32& OutStatus);
	EAvidScriptVmTypedHostStatus
		DispatchPreparedReflectionStableObjectRoundtrip(
			FAvidScriptPreparedReflectionHostCall& Call,
			int32 SelfSlot,
			int32 SelfGeneration,
			int32 ObjectSlot,
			int32 ObjectGeneration,
			int32 GuestAddress,
			int32& OutStatus);
	bool ResolvePreparedReflectionCallMode(
		FAvidScriptPreparedReflectionHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		UObject*& OutReceiver,
		bool& bOutUseNative,
		EAvidScriptBindingInvocationMode& OutMode,
		bool& bOutAdaptiveGuardRejected);
	void RecordPreparedReflectionInvocation(
		EAvidScriptBindingInvocationMode Mode,
		bool bAdaptiveGuardRejected);
	static EAvidScriptVmTypedHostStatus InvokePreparedSelfPropertyI32Get(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32& OutValue);
	static int32 InvokePreparedDirectSelfPropertyI32Get(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32* OutValue);
	EAvidScriptVmTypedHostStatus DispatchPreparedSelfPropertyI32Get(
		FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32& OutValue);
	static EAvidScriptVmTypedHostStatus InvokePreparedSelfPropertyI32Set(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value);
	static int32 InvokePreparedDirectSelfPropertyI32Set(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value,
		int32* OutValue);
	EAvidScriptVmTypedHostStatus DispatchPreparedSelfPropertyI32Set(
		FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value);
	static EAvidScriptVmTypedHostStatus InvokePreparedSelfVectorRefOut(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue);
	EAvidScriptVmTypedHostStatus DispatchPreparedSelfVectorRefOut(
		FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue);
	static EAvidScriptVmTypedHostStatus InvokePreparedStableObjectRoundtrip(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 ObjectSlot,
		int32 ObjectGeneration,
		int32 GuestAddress,
		int32& OutValue);
	EAvidScriptVmTypedHostStatus DispatchPreparedStableObjectRoundtrip(
		FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 ObjectSlot,
		int32 ObjectGeneration,
		int32 GuestAddress,
		int32& OutValue);
	static EAvidScriptVmTypedHostStatus
		InvokePreparedPackedStableObjectRoundtrip(
			void* Context,
			int64 PackedSelf,
			int64 PackedObject,
			int64& OutPackedObject);
	EAvidScriptVmTypedHostStatus DispatchPreparedPackedStableObjectRoundtrip(
		FAvidScriptPreparedGeneratedHostCall& Call,
		int64 PackedSelf,
		int64 PackedObject,
		int64& OutPackedObject);
	EAvidScriptVmTypedHostStatus DispatchPreparedObjectRoundtripCore(
		FAvidScriptPreparedGeneratedHostCall& Call,
		const FAvidScriptObjectHandle& ReceiverHandle,
		const FAvidScriptObjectHandle& InputHandle,
		FAvidScriptObjectHandle& OutHandle);
	bool TryResolveFusedCallbackReceiver(
		int32 SelfSlot,
		int32 SelfGeneration,
		UObject*& OutReceiver);
	bool PrepareFusedGeneratedHostEffect(
		FAvidScriptPreparedGeneratedHostCall& Call,
		UObject& Receiver);
	void BeginTypedCallbackEpoch();
	void EndTypedCallbackEpoch();
	void InvalidateSelfCapability();
	bool ResolveSelfCapability(
		int32 SelfSlot,
		int32 SelfGeneration,
		UClass* ExpectedClass,
		UObject*& OutObject);
	UObject* ResolveStableBorrow(
		int32 Slot,
		int32 Generation,
		UClass* ExpectedClass) const;
	EAvidScriptVmTypedHostStatus RecordGeneratedStatus(
		EAvidScriptVmTypedHostStatus Status);
	void CollectDueTimers(float DeltaSeconds);
	bool ExecuteDueTimerCallbacks(FAvidScriptVmError& OutError);
	void RequeueDueTimerCallbacks(int32 FirstTimerIndex);
	bool IsDebugExecutionSuspended() const;
	bool DispatchEvent(
		int32 EventId,
		float Value,
		FAvidScriptWasmSmokeResult& OutResult,
		EAvidScriptWasmResultDetail ResultDetail);
	bool DispatchGameplayEvent(
		const FAvidScriptGameplayEvent& Event,
		FAvidScriptWasmSmokeResult& OutResult,
		EAvidScriptWasmResultDetail ResultDetail);
	int32 AllocateTimerHandle();
	void CompactTimerHeapIfNeeded();
	void ResetTimerState();
	void CopyTimerStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;
	void ResetEventState();
	void CopyEventStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;
	void ClearPendingHostImportFailure();
	void ResetHostImportState();
	void CopyHostImportStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;
	void CopyObservableStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;
	bool PrepareDelegateEventExports(
		TArray<FAvidScriptPreparedDelegateEvent>& InOutEvents,
		FString& OutError);

    FAvidScriptWasmInstanceExecutionState DefaultInstanceState;
    FAvidScriptWasmInstanceExecutionState* ActiveInstanceState = &DefaultInstanceState;
    TArray<TWeakPtr<FAvidScriptWasmInstanceExecutionState>> InstanceExecutionStates;
    FAvidScriptWasmInstanceExecutionState& GetInstanceState() { return *ActiveInstanceState; }
    const FAvidScriptWasmInstanceExecutionState& GetInstanceState() const { return *ActiveInstanceState; }
	TUniquePtr<IAvidScriptVmBackend> VmBackend;
	TSharedPtr<const uint8> ContextCallCodeIdentity;
	uint32 ContextInvocationDepth = 0;
	uint32 ContextInvocationEntries = 0;
	FAvidScriptVmError ContextInvocationFailure;
	TUniquePtr<AvidScript::Managed::FHeap> ManagedHeap;
	uint32 ManagedHeapFrameFloor = 0;
	uint32 ManagedHeapInvocationDepth = 0;
	const FAvidScriptManagedRootTransfer* ActiveRootTransfer = nullptr;
	FAvidScriptVmBackendSelection BackendSelection;
	FAvidScriptVmBackendInfo ActiveBackendInfo;
	FAvidScriptVmLoadConfig::FExecutionBudget ExecutionBudget;
	FAvidScriptCachedVmExport BeginPlayExport;
	// Resolved once during Load; an invalid handle on a loaded instance means no guest Tick.
	FAvidScriptCachedVmExport TickExport;
	FAvidScriptCachedVmExport EndPlayExport;
	FAvidScriptCachedVmExport TimerExport;
	FAvidScriptCachedVmExport ContinuationExport;
	FAvidScriptCachedVmExport ContinuationV2Export;
	FAvidScriptCachedVmExport DebugResumeExport;
	FAvidScriptCachedVmExport EventExport;
	FAvidScriptCachedVmExport GameplayEventExport;
	TMap<FString, FAvidScriptCachedVmExport> DelegateEventExports;

	bool bGameplayEventExportLookupAttempted = false;
#if WITH_DEV_AUTOMATION_TESTS
	FAvidScriptCachedVmExport TestingI32PairExport;
	FString TestingI32PairExportName;
	int32 StateWriteAttemptCount = 0;
	TArray<int32> StateWriteFailureAttempts;
#endif
	bool bHasPendingHostImportFailure = false;
	uint64 PendingHostImportCallbackEpoch = 0;
	FString PendingHostImportModuleName;
	FString PendingHostImportName;
	FString PendingHostImportErrorCategory;
	FString PendingHostImportDetails;
	FAvidScriptPreparedDelegateOutputTransaction*
		ActiveDelegateOutputTransaction = nullptr;
	uint32 ActiveDelegateOutputToken = 0;
	bool bContinuationDispatchActive = false;
	bool bContinuationResultConsumed = false;
	bool bContinuationStateConsumed = false;
	int64 ActiveContinuationToken = 0;
	EAvidScriptContinuationStatus ActiveContinuationStatus =
		EAvidScriptContinuationStatus::Failed;
	int32 ActiveContinuationResultSlot = 0;
	int32 ActiveContinuationResultGeneration = 0;
	FAvidScriptContinuationResultCodecTransaction*
		ActiveContinuationResultTransaction = nullptr;
	FString ModuleId;
	FAvidScriptWasmHostContext HostContext;
	FAvidScriptArrayValueHeap ArrayValueHeap;
	FAvidScriptUtf8ValueHeap Utf8ValueHeap;
	FAvidScriptCompositeValueHeap CompositeValueHeap;
	FAvidScriptBindingInvocationContext BindingInvocationContext;
	TSharedPtr<const FAvidScriptBindingPackage> BindingPackage;
	TSharedPtr<const FAvidScriptWasmDebugMap> DebugMap;
	TArray<FAvidScriptVmTypedHostImport> SupplementalTypedHostImports;
	TSharedPtr<FAvidScriptGeneratedTypeHostBindings> GeneratedTypeHostBindings;
	TArray<FAvidScriptVmTypedHostImport> TypedHostImports;
	TArray<TUniquePtr<FAvidScriptPreparedGeneratedHostCall>>
		PreparedGeneratedHostCalls;
	TArray<TUniquePtr<FAvidScriptPreparedReflectionHostCall>>
		PreparedReflectionHostCalls;
	FAvidScriptVmBindingPackage PreparedVmBindingPackage;
	TArray<TUniquePtr<FAvidScriptPreparedDynamicHostCall>>
		PreparedDynamicHostCalls;
	TArray<uint8> BindingInvocationScratch;
	TArray<FAvidScriptObjectHandle> TransformBatchHandleScratch;
	TArray<FAvidScriptActorTransformSnapshot> TransformBatchSnapshotScratch;
	TArray<float> TransformBatchOutputScratch;
	FAvidScriptWasmRuntimeMetrics Metrics;
	FAvidScriptSelfCapability SelfCapability;
	FAvidScriptDataBridgeBudget DataBridgeBudget;
	FAvidScriptDataBridgeMetrics DataBridgeMetrics;
	TArray<FAvidScriptFusedCallbackFrame, TInlineAllocator<4>>
		FusedCallbackFrameStack;
	uint64 NextCallbackEpoch = 0;
	uint64 ReloadEpoch = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntime
{
public:
	static bool RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
	static bool RunEmbeddedHostImportSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
};
