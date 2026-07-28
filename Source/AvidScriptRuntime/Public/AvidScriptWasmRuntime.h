#pragma once

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptDataBridgeTypes.h"
#include "AvidScriptWasmDiagnostics.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptLifecycleState.h"
#include "AvidScriptVmBackend.h"
#include "AvidScriptActorBinding.h"
#include "AvidScriptObjectOwnership.h"

#include "CoreMinimal.h"

class FAvidScriptWasmDebugMap;
class UWorld;

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
	FullSnapshot
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

struct FAvidScriptWasmHostContext
{
	FAvidScriptObjectRegistry* ObjectRegistry = nullptr;
	IAvidScriptObjectOwnershipDomain* ObjectOwnership = nullptr;
	FAvidScriptObjectHandle OwnerHandle;
	TWeakObjectPtr<UWorld> World;
	EAvidScriptActorWritePolicy ActorWritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
	IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr;
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

struct FAvidScriptSelfCapability
{
	TWeakObjectPtr<UObject> Object;
	FAvidScriptObjectHandle Handle;
	uint64 ReloadEpoch = 0;
	uint64 CallbackEpoch = 0;
	uint64 RegistryRevision = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntimeInstance
	: public IAvidScriptHostDispatcher
	, public IAvidScriptVmTypedHostDispatcher
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
	bool ValidateRequiredExports(
		const TArray<FString>& RequiredExports,
		FAvidScriptWasmSmokeResult& OutResult);
	bool BeginPlay(FAvidScriptWasmSmokeResult& OutResult);
	bool Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool Tick(
		float DeltaSeconds,
		FAvidScriptWasmSmokeResult& OutResult,
		EAvidScriptWasmResultDetail ResultDetail);
	bool EndPlay(FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEvent(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchGameplayEvent(const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult);
	void Unload();
	void Unload(FAvidScriptWasmSmokeResult& OutResult);
	bool ReadStateBytes(uint32 GuestAddress, TArrayView<uint8> OutBytes, FString& OutError) const;
	bool WriteStateBytes(uint32 GuestAddress, TConstArrayView<uint8> Bytes, FString& OutError);
#if WITH_DEV_AUTOMATION_TESTS
	void SetStateWriteFailuresForTesting(TConstArrayView<int32> InWriteAttempts);
	void ClearStateWriteFailureForTesting();
	void BeginTypedCallbackEpochForTesting() { BeginTypedCallbackEpoch(); }
	void EndTypedCallbackEpochForTesting() { EndTypedCallbackEpoch(); }
	void SetBindingPackageForTesting(
		const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage)
	{
		BindingPackage = InBindingPackage;
	}
	uint64 GetActiveCallbackEpochForTesting() const
	{
		return CallbackEpochStack.IsEmpty() ? 0 : CallbackEpochStack.Last();
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
	int32 GetHostImportCallCountForTesting() const { return HostImportCallCount; }
	EAvidScriptVmTypedHostStatus RecordGeneratedStatusForTesting(
		EAvidScriptVmTypedHostStatus Status)
	{
		return RecordGeneratedStatus(Status);
	}
#endif

	bool IsLoaded() const;
	EAvidScriptLifecycleState GetLifecycleState() const { return LifecycleState.GetState(); }
	bool HasBegunPlay() const { return bHasBegunPlay; }
	int32 GetTickCallCount() const { return TickCallCount; }
	int32 GetPendingTimerCount() const { return ActiveTimers.Num(); }
	int32 GetTimerCallbackCount() const { return TimerCallbackCount; }
	int32 GetEventCallbackCount() const { return EventCallbackCount; }
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
	int32 HandleTimerSetOnceImport(float DelaySeconds, int32 CallbackId);
	int32 HandleTimerCancelImport(int32 TimerHandle);
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
		const FString& Details);
	bool ConsumePendingHostImportFailure(FString& OutImportModuleName, FString& OutImportName, FString& OutDetails);
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) override;
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
	bool BuildPreparedTypedHostImports(FString& OutError);
	static EAvidScriptVmTypedHostStatus InvokePreparedSelfI32Pair(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue);
	EAvidScriptVmTypedHostStatus DispatchPreparedSelfI32Pair(
		const FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue);
	static EAvidScriptVmTypedHostStatus InvokePreparedSelfPropertyI32Get(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32& OutValue);
	EAvidScriptVmTypedHostStatus DispatchPreparedSelfPropertyI32Get(
		const FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32& OutValue);
	static EAvidScriptVmTypedHostStatus InvokePreparedSelfPropertyI32Set(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value);
	EAvidScriptVmTypedHostStatus DispatchPreparedSelfPropertyI32Set(
		const FAvidScriptPreparedGeneratedHostCall& Call,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value);
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
	bool ExecuteDueTimerCallbacks(FAvidScriptWasmSmokeResult& OutResult);
	int32 AllocateTimerHandle();
	void CompactTimerHeapIfNeeded();
	void ResetTimerState();
	void CopyTimerStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;
	void ResetEventState();
	void CopyEventStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;
	void ResetHostImportState();
	void CopyHostImportStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;
	void CopyObservableStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;

	TUniquePtr<IAvidScriptVmBackend> VmBackend;
	FAvidScriptVmBackendSelection BackendSelection;
	FAvidScriptVmBackendInfo ActiveBackendInfo;
	FAvidScriptVmExportHandle BeginPlayExport;
	FAvidScriptVmExportHandle TickExport;
	FAvidScriptVmExportHandle EndPlayExport;
	FAvidScriptVmExportHandle TimerExport;
	FAvidScriptVmExportHandle EventExport;
	FAvidScriptVmExportHandle GameplayEventExport;

	bool bGameplayEventExportLookupAttempted = false;
	bool bHasBegunPlay = false;
#if WITH_DEV_AUTOMATION_TESTS
	int32 StateWriteAttemptCount = 0;
	TArray<int32> StateWriteFailureAttempts;
#endif
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
	bool bHasPendingHostImportFailure = false;
	FString PendingHostImportModuleName;
	FString PendingHostImportName;
	FString PendingHostImportDetails;
	FString ModuleId;
	FAvidScriptWasmSmokeResult CachedEndPlayResult;
	FAvidScriptWasmHostContext HostContext;
	FAvidScriptBindingInvocationContext BindingInvocationContext;
	TSharedPtr<const FAvidScriptBindingPackage> BindingPackage;
	TSharedPtr<const FAvidScriptWasmDebugMap> DebugMap;
	TArray<FAvidScriptVmTypedHostImport> TypedHostImports;
	TArray<TUniquePtr<FAvidScriptPreparedGeneratedHostCall>>
		PreparedGeneratedHostCalls;
	TArray<uint8> BindingInvocationScratch;
	TArray<FAvidScriptObjectHandle> TransformBatchHandleScratch;
	TArray<FAvidScriptActorTransformSnapshot> TransformBatchSnapshotScratch;
	TArray<float> TransformBatchOutputScratch;
	FAvidScriptLifecycleStateMachine LifecycleState;
	FAvidScriptWasmRuntimeMetrics Metrics;
	FAvidScriptSelfCapability SelfCapability;
	FAvidScriptDataBridgeBudget DataBridgeBudget;
	FAvidScriptDataBridgeMetrics DataBridgeMetrics;
	TArray<uint64, TInlineAllocator<4>> CallbackEpochStack;
	uint64 NextCallbackEpoch = 0;
	uint64 ReloadEpoch = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntime
{
public:
	static bool RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
	static bool RunEmbeddedHostImportSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
};
