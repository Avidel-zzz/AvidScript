#pragma once

#include "CoreMinimal.h"

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
	bool BeginPlay(FAvidScriptWasmSmokeResult& OutResult);
	bool Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	void Unload();

	bool IsLoaded() const;
	bool HasBegunPlay() const { return bHasBegunPlay; }
	int32 GetTickCallCount() const { return TickCallCount; }
	const FString& GetModuleId() const { return ModuleId; }

private:
	void* Module = nullptr;
	void* ModuleInstance = nullptr;
	void* ExecEnv = nullptr;
	bool bOwnsRuntimeLease = false;
	bool bHasBegunPlay = false;
	int32 TickCallCount = 0;
	FString ModuleId;
	TArray<uint8> ModuleBuffer;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmRuntime
{
public:
	static bool RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult);
};
