#pragma once

#include "CoreMinimal.h"

struct FAvidScriptWasmRuntimeMetrics
{
	double RuntimeInitMs = 0.0;
	double ModuleLoadMs = 0.0;
	double ModuleInstantiateMs = 0.0;
	double ExecEnvCreateMs = 0.0;
	double BeginPlayCallMs = 0.0;
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
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntimeInstance
{
public:
	FAvidScriptWasmRuntimeInstance() = default;
	~FAvidScriptWasmRuntimeInstance();

	FAvidScriptWasmRuntimeInstance(const FAvidScriptWasmRuntimeInstance&) = delete;
	FAvidScriptWasmRuntimeInstance& operator=(const FAvidScriptWasmRuntimeInstance&) = delete;

	bool LoadEmbeddedSmokeModule(FAvidScriptWasmSmokeResult& OutResult);
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

private:
	void* Module = nullptr;
	void* ModuleInstance = nullptr;
	void* ExecEnv = nullptr;
	bool bOwnsRuntimeLease = false;
	bool bHasBegunPlay = false;
	int32 TickCallCount = 0;
	FString ModuleId;
	TArray<uint8> ModuleBuffer;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntime
{
public:
	static bool RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
};
