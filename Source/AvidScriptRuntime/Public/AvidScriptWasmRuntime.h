#pragma once

#include "AvidScriptActorBinding.h"

#include "CoreMinimal.h"

struct FAvidScriptWasmRuntimeMetrics
{
	double RuntimeInitMs = 0.0;
	double ModuleLoadMs = 0.0;
	double ModuleInstantiateMs = 0.0;
	double ExecEnvCreateMs = 0.0;
	double BeginPlayCallMs = 0.0;
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
	bool bTickCalled = false;
	bool bUnloaded = false;
	int32 TickCallCount = 0;
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
	EAvidScriptActorWritePolicy ActorWritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntimeInstance
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
	void Unload();
	void Unload(FAvidScriptWasmSmokeResult& OutResult);

	bool IsLoaded() const;
	bool HasBegunPlay() const { return bHasBegunPlay; }
	int32 GetTickCallCount() const { return TickCallCount; }
	const FString& GetModuleId() const { return ModuleId; }
	const FAvidScriptWasmRuntimeMetrics& GetMetrics() const { return Metrics; }
	void SetHostContext(const FAvidScriptWasmHostContext& InHostContext);
	void ClearHostContext();
	int32 HandleHostAddI32Import(int32 Input);
	int32 HandleHostFailI32Import(int32 Input);
	int32 HandleActorGetLocationImport(int32 Slot, int32 Generation, FVector& OutLocation);
	int32 HandleActorSetLocationImport(int32 Slot, int32 Generation, const FVector& Location);
	int32 HandleActorAddLocationOffsetImport(int32 Slot, int32 Generation, const FVector& Offset);
	void SetPendingHostImportFailure(
		const FString& ImportModuleName,
		const FString& ImportName,
		const FString& Details);
	bool ConsumePendingHostImportFailure(FString& OutImportModuleName, FString& OutImportName, FString& OutDetails);

private:
	void ResetHostImportState();
	void CopyHostImportStateToResult(FAvidScriptWasmSmokeResult& OutResult) const;

	void* Module = nullptr;
	void* ModuleInstance = nullptr;
	void* ExecEnv = nullptr;
	bool bOwnsRuntimeLease = false;
	bool bHasBegunPlay = false;
	int32 TickCallCount = 0;
	int32 HostImportCallCount = 0;
	int32 LastHostImportInput = 0;
	int32 LastHostImportResult = 0;
	bool bHasPendingHostImportFailure = false;
	FString PendingHostImportModuleName;
	FString PendingHostImportName;
	FString PendingHostImportDetails;
	FString ModuleId;
	TArray<uint8> ModuleBuffer;
	FAvidScriptWasmHostContext HostContext;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntime
{
public:
	static bool RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
	static bool RunEmbeddedHostImportSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
};
