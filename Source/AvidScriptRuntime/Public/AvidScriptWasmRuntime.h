#pragma once

#include "AvidScriptLifecycleState.h"
#include "AvidScriptVmBackend.h"
#include "AvidScriptActorBinding.h"

#include "CoreMinimal.h"

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
	double TickCallMs = 0.0;
	double UnloadMs = 0.0;
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
	int32 HostImportCallCount = 0;
	int32 LastHostImportInput = 0;
	int32 LastHostImportResult = 0;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

struct FAvidScriptWasmHostContext
{
	FAvidScriptObjectRegistry* ObjectRegistry = nullptr;
	FAvidScriptObjectHandle OwnerHandle;
	EAvidScriptActorWritePolicy ActorWritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
};

struct FAvidScriptWasmTimerEntry
{
	int32 Handle = 0;
	int32 CallbackId = 0;
	double DueTimeSeconds = 0.0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntimeInstance : public IAvidScriptHostDispatcher
{
public:
	FAvidScriptWasmRuntimeInstance() = default;
	~FAvidScriptWasmRuntimeInstance();

	FAvidScriptWasmRuntimeInstance(const FAvidScriptWasmRuntimeInstance&) = delete;
	FAvidScriptWasmRuntimeInstance& operator=(const FAvidScriptWasmRuntimeInstance&) = delete;

	bool LoadEmbeddedSmokeModule(FAvidScriptWasmSmokeResult& OutResult);
	bool LoadEmbeddedHostImportModule(FAvidScriptWasmSmokeResult& OutResult);
	bool LoadModule(const uint8* Bytecode, int32 BytecodeSize, const FString& InModuleId, FAvidScriptWasmSmokeResult& OutResult);
	bool ValidateRequiredExports(const TArray<FString>& RequiredExports, FAvidScriptWasmSmokeResult& OutResult) const;
	bool BeginPlay(FAvidScriptWasmSmokeResult& OutResult);
	bool Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool EndPlay(FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEvent(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
	void Unload();
	void Unload(FAvidScriptWasmSmokeResult& OutResult);

	bool IsLoaded() const;
	EAvidScriptLifecycleState GetLifecycleState() const { return LifecycleState.GetState(); }
	bool HasBegunPlay() const { return bHasBegunPlay; }
	int32 GetTickCallCount() const { return TickCallCount; }
	int32 GetPendingTimerCount() const { return ActiveTimers.Num(); }
	int32 GetTimerCallbackCount() const { return TimerCallbackCount; }
	int32 GetEventCallbackCount() const { return EventCallbackCount; }
	const FString& GetModuleId() const { return ModuleId; }
	const FAvidScriptWasmRuntimeMetrics& GetMetrics() const { return Metrics; }
	void SetHostContext(const FAvidScriptWasmHostContext& InHostContext);
	void ClearHostContext();
	int32 HandleHostAddI32Import(int32 Input);
	int32 HandleHostFailI32Import(int32 Input);
	int32 HandleOwnerGetSlotImport();
	int32 HandleOwnerGetGenerationImport();
	int32 HandleTimerSetOnceImport(float DelaySeconds, int32 CallbackId);
	int32 HandleTimerCancelImport(int32 TimerHandle);
	int32 HandleActorGetLocationImport(int32 Slot, int32 Generation, FVector& OutLocation);
	int32 HandleActorSetLocationImport(int32 Slot, int32 Generation, const FVector& Location);
	int32 HandleActorAddLocationOffsetImport(int32 Slot, int32 Generation, const FVector& Offset);
	int32 HandleActorGetRotationImport(int32 Slot, int32 Generation, FRotator& OutRotation);
	int32 HandleActorSetRotationImport(int32 Slot, int32 Generation, const FRotator& Rotation);
	int32 HandleActorGetScaleImport(int32 Slot, int32 Generation, FVector& OutScale3D);
	int32 HandleActorSetScaleImport(int32 Slot, int32 Generation, const FVector& Scale3D);
	int32 HandleActorGetRootComponentImport(int32 Slot, int32 Generation, FAvidScriptObjectHandle& OutComponentHandle);
	int32 HandleSceneComponentGetWorldLocationImport(int32 Slot, int32 Generation, FVector& OutWorldLocation);
	int32 HandleSceneComponentSetWorldLocationImport(int32 Slot, int32 Generation, const FVector& WorldLocation);
	void SetPendingHostImportFailure(
		const FString& ImportModuleName,
		const FString& ImportName,
		const FString& Details);
	bool ConsumePendingHostImportFailure(FString& OutImportModuleName, FString& OutImportName, FString& OutDetails);
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) override;


private:
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

	TUniquePtr<IAvidScriptVmBackend> VmBackend;
	FAvidScriptVmExportHandle BeginPlayExport;
	FAvidScriptVmExportHandle TickExport;
	FAvidScriptVmExportHandle EndPlayExport;
	FAvidScriptVmExportHandle TimerExport;
	FAvidScriptVmExportHandle EventExport;

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
	bool bHasPendingHostImportFailure = false;
	FString PendingHostImportModuleName;
	FString PendingHostImportName;
	FString PendingHostImportDetails;
	FString ModuleId;
	FAvidScriptWasmSmokeResult CachedEndPlayResult;
	FAvidScriptWasmHostContext HostContext;
	FAvidScriptLifecycleStateMachine LifecycleState;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntime
{
public:
	static bool RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
	static bool RunEmbeddedHostImportSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
};
