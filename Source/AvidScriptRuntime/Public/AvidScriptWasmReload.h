#pragma once

#include "AvidScriptWasmRuntime.h"

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmRequiredImport
{
	FString ModuleName;
	FString ImportName;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadManifest
{
	static constexpr int32 SupportedSchemaVersion = 1;
	static constexpr int32 SupportedAbiVersion = 1;

	FString ModuleId;
	int32 AbiVersion = SupportedAbiVersion;
	FString Language;
	FString WasmFile;
	FString WasmSha256;
	TArray<FString> RequiredExports;
	TArray<FAvidScriptWasmRequiredImport> RequiredImports;

	static FAvidScriptWasmReloadManifest MakeSmoke(const FString& InModuleId);
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadManifestLoadResult
{
	bool bSucceeded = false;
	FString ManifestPath;
	FString ModulePath;
	int64 ByteSize = 0;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadManifestLoader
{
public:
	static bool LoadFromFile(
		const FString& ManifestPath,
		FAvidScriptWasmReloadManifest& OutManifest,
		TArray<uint8>& OutBytecode,
		FAvidScriptWasmReloadManifestLoadResult& OutResult);
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
	bool StopAndUnload(FAvidScriptWasmSmokeResult& OutResult);
	bool TickLive(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult);
	bool DispatchEventLive(int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult);
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
		FAvidScriptWasmReloadResult& OutResult);

	TUniquePtr<FAvidScriptWasmRuntimeInstance> LiveRuntime;
	FAvidScriptWasmReloadManifest LiveManifest;
	FAvidScriptWasmHostContext HostContext;
	int32 SuccessfulReloadCount = 0;
	int32 RejectedReloadCount = 0;
};

using FAvidScriptWasmReloadSession = FAvidScriptRuntimeSession;
