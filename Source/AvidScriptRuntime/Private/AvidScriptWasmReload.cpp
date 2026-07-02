#include "AvidScriptWasmReload.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWasmReload, Log, All);

namespace
{
void ResetReloadResult(
	FAvidScriptWasmReloadResult& OutResult,
	const FString& PreviousModuleId,
	const FString& CandidateModuleId,
	const FString& ActiveModuleId)
{
	OutResult = FAvidScriptWasmReloadResult();
	OutResult.PreviousModuleId = PreviousModuleId;
	OutResult.CandidateModuleId = CandidateModuleId;
	OutResult.ActiveModuleId = ActiveModuleId;
}

void CopyRuntimeFailure(
	const FAvidScriptWasmSmokeResult& RuntimeResult,
	FAvidScriptWasmReloadResult& OutResult)
{
	OutResult.RuntimeResult = RuntimeResult;
	OutResult.ExportName = RuntimeResult.ExportName;
	OutResult.ErrorCategory = RuntimeResult.ErrorCategory;
	OutResult.NextAction = RuntimeResult.NextAction;
	OutResult.ErrorMessage = RuntimeResult.ErrorMessage;
}

void SetReloadFailure(
	FAvidScriptWasmReloadResult& OutResult,
	const FString& ExportName,
	const FString& Category,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = Category;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript reload rejected | previous=%s | candidate=%s | active=%s | export=%s | category=%s | details=%s | next=%s"),
		OutResult.PreviousModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.PreviousModuleId,
		OutResult.CandidateModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.CandidateModuleId,
		OutResult.ActiveModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.ActiveModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
		*Category,
		*Details,
		*NextAction);

	UE_LOG(LogAvidScriptWasmReload, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}

void MarkRejectedReloadWithRollback(
	const FString& ActiveModuleId,
	FAvidScriptWasmReloadResult& OutResult)
{
	OutResult.ActiveModuleId = ActiveModuleId;
	OutResult.bRollbackPreservedLiveRuntime = !ActiveModuleId.IsEmpty();
}
} // namespace

FAvidScriptWasmReloadManifest FAvidScriptWasmReloadManifest::MakeSmoke(const FString& InModuleId)
{
	FAvidScriptWasmReloadManifest Manifest;
	Manifest.ModuleId = InModuleId;
	Manifest.AbiVersion = SupportedAbiVersion;
	Manifest.RequiredExports = {
		TEXT("avid_on_begin_play"),
		TEXT("avid_on_tick")
	};
	return Manifest;
}

bool FAvidScriptWasmReloadSession::LoadInitialModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, Manifest.ModuleId, PreviousModuleId);

	if (!ValidateManifest(Manifest, PreviousModuleId, OutResult))
	{
		return false;
	}

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
	if (!BuildValidatedRuntime(Bytecode, BytecodeSize, Manifest, CandidateRuntime, OutResult))
	{
		OutResult.ActiveModuleId = PreviousModuleId;
		return false;
	}

	LiveRuntime = MoveTemp(CandidateRuntime);
	LiveManifest = Manifest;

	OutResult.bSucceeded = true;
	OutResult.ActiveModuleId = Manifest.ModuleId;
	return true;
}

bool FAvidScriptWasmReloadSession::ReloadModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, Manifest.ModuleId, PreviousModuleId);

	if (!ValidateManifest(Manifest, PreviousModuleId, OutResult))
	{
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
	if (!BuildValidatedRuntime(Bytecode, BytecodeSize, Manifest, CandidateRuntime, OutResult))
	{
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	LiveRuntime = MoveTemp(CandidateRuntime);
	LiveManifest = Manifest;
	++SuccessfulReloadCount;

	OutResult.bSucceeded = true;
	OutResult.bReloadApplied = true;
	OutResult.ActiveModuleId = Manifest.ModuleId;
	return true;
}

bool FAvidScriptWasmReloadSession::TickLive(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	if (!IsLiveLoaded())
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ModuleId = LiveManifest.ModuleId;
		OutResult.ExportName = TEXT("avid_on_tick");
		OutResult.ErrorCategory = TEXT("invalid_state");
		OutResult.NextAction = TEXT("load a validated WASM module before ticking the live script runtime");
		OutResult.ErrorMessage = FString::Printf(
			TEXT("AvidScript live runtime tick rejected | module=%s | category=invalid_state | details=no live runtime is loaded"),
			LiveManifest.ModuleId.IsEmpty() ? TEXT("<none>") : *LiveManifest.ModuleId);
		return false;
	}

	return LiveRuntime->Tick(DeltaSeconds, OutResult);
}

void FAvidScriptWasmReloadSession::UnloadLive()
{
	if (LiveRuntime)
	{
		LiveRuntime->Unload();
		LiveRuntime.Reset();
	}
	LiveManifest = FAvidScriptWasmReloadManifest();
}

bool FAvidScriptWasmReloadSession::IsLiveLoaded() const
{
	return LiveRuntime.IsValid() && LiveRuntime->IsLoaded();
}

FString FAvidScriptWasmReloadSession::GetLiveModuleId() const
{
	return IsLiveLoaded() ? LiveRuntime->GetModuleId() : FString();
}

int32 FAvidScriptWasmReloadSession::GetLiveTickCallCount() const
{
	return IsLiveLoaded() ? LiveRuntime->GetTickCallCount() : 0;
}

bool FAvidScriptWasmReloadSession::ValidateManifest(
	const FAvidScriptWasmReloadManifest& Manifest,
	const FString& PreviousModuleId,
	FAvidScriptWasmReloadResult& OutResult) const
{
	OutResult.PreviousModuleId = PreviousModuleId;
	OutResult.CandidateModuleId = Manifest.ModuleId;

	if (Manifest.ModuleId.IsEmpty())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("manifest_invalid"),
			TEXT("manifest ModuleId is empty"),
			TEXT("assign a stable module id before loading or reloading a script"));
		return false;
	}

	if (Manifest.AbiVersion != FAvidScriptWasmReloadManifest::SupportedAbiVersion)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("abi_mismatch"),
			FString::Printf(
				TEXT("manifest ABI version %d does not match supported ABI version %d"),
				Manifest.AbiVersion,
				FAvidScriptWasmReloadManifest::SupportedAbiVersion),
			TEXT("rebuild the script with the runtime supported ABI or keep the previous live runtime"));
		return false;
	}

	if (Manifest.RequiredExports.IsEmpty())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("manifest_invalid"),
			TEXT("manifest RequiredExports is empty"),
			TEXT("declare the guest exports that must be validated before activation"));
		return false;
	}

	return true;
}

bool FAvidScriptWasmReloadSession::BuildValidatedRuntime(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	TUniquePtr<FAvidScriptWasmRuntimeInstance>& OutRuntime,
	FAvidScriptWasmReloadResult& OutResult) const
{
	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime = MakeUnique<FAvidScriptWasmRuntimeInstance>();
	FAvidScriptWasmSmokeResult RuntimeResult;

	if (!CandidateRuntime->LoadModule(Bytecode, BytecodeSize, Manifest.ModuleId, RuntimeResult))
	{
		CopyRuntimeFailure(RuntimeResult, OutResult);
		return false;
	}

	if (!CandidateRuntime->ValidateRequiredExports(Manifest.RequiredExports, RuntimeResult))
	{
		CopyRuntimeFailure(RuntimeResult, OutResult);
		CandidateRuntime->Unload();
		return false;
	}

	if (!CandidateRuntime->BeginPlay(RuntimeResult))
	{
		CopyRuntimeFailure(RuntimeResult, OutResult);
		CandidateRuntime->Unload();
		return false;
	}

	OutResult.RuntimeResult = RuntimeResult;
	OutRuntime = MoveTemp(CandidateRuntime);
	return true;
}
