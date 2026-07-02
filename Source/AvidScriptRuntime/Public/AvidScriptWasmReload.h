#pragma once

#include "AvidScriptWasmRuntime.h"

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadManifest
{
	static constexpr int32 SupportedAbiVersion = 1;

	FString ModuleId;
	int32 AbiVersion = SupportedAbiVersion;
	TArray<FString> RequiredExports;

	static FAvidScriptWasmReloadManifest MakeSmoke(const FString& InModuleId);
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadResult
{
	bool bSucceeded = false;
	bool bReloadApplied = false;
	bool bRollbackPreservedLiveRuntime = false;
	FString PreviousModuleId;
	FString CandidateModuleId;
	FString ActiveModuleId;
	FString ExportName;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
	FAvidScriptWasmSmokeResult RuntimeResult;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadSession
{
public:
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

	bool TickLive(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	void UnloadLive();

	bool IsLiveLoaded() const;
	FString GetLiveModuleId() const;
	int32 GetLiveTickCallCount() const;
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

	TUniquePtr<FAvidScriptWasmRuntimeInstance> LiveRuntime;
	FAvidScriptWasmReloadManifest LiveManifest;
	int32 SuccessfulReloadCount = 0;
	int32 RejectedReloadCount = 0;
};
