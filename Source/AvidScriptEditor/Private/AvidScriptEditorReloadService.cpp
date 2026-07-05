#include "AvidScriptEditorReloadService.h"

namespace
{
void SetAvidScriptReloadApplyFailure(
	const EAvidScriptEditorReloadApplyStatus Status,
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorReloadApplyResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.bApplied = false;
	OutResult.bInitialLoad = false;
	OutResult.bReload = false;
	OutResult.Status = Status;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}

void CopyAvidScriptReloadRuntimeFailure(
	const FAvidScriptWasmReloadResult& RuntimeResult,
	FAvidScriptEditorReloadApplyResult& OutResult)
{
	SetAvidScriptReloadApplyFailure(
		EAvidScriptEditorReloadApplyStatus::FailedRuntime,
		RuntimeResult.ErrorCategory,
		RuntimeResult.ErrorMessage,
		RuntimeResult.NextAction,
		OutResult);
	OutResult.RuntimeResult = RuntimeResult;
}
} // namespace

bool FAvidScriptEditorReloadService::ApplyCompileResult(
	const FAvidScriptEditorCompileResult& CompileResult,
	FAvidScriptWasmReloadSession& ReloadSession,
	FAvidScriptEditorReloadApplyResult& OutResult)
{
	OutResult = FAvidScriptEditorReloadApplyResult();

	if (CompileResult.Status == EAvidScriptEditorCompileStatus::SucceededGeneratedOnly)
	{
		OutResult.bSucceeded = true;
		OutResult.Status = EAvidScriptEditorReloadApplyStatus::SkippedGeneratedOnly;
		OutResult.NextAction = TEXT("build a WASM artifact and manifest before applying reload");
		return true;
	}

	if (!CompileResult.bSucceeded || !CompileResult.bReloadable)
	{
		SetAvidScriptReloadApplyFailure(
			EAvidScriptEditorReloadApplyStatus::RejectedCompileResult,
			CompileResult.ErrorCategory.IsEmpty() ? TEXT("compile_result_not_reloadable") : CompileResult.ErrorCategory,
			CompileResult.ErrorMessage.IsEmpty()
				? TEXT("AvidScript compile result is not reloadable.")
				: CompileResult.ErrorMessage,
			CompileResult.NextAction,
			OutResult);
		return false;
	}

	if (CompileResult.Bytecode.IsEmpty())
	{
		SetAvidScriptReloadApplyFailure(
			EAvidScriptEditorReloadApplyStatus::RejectedCompileResult,
			TEXT("bytecode_missing"),
			TEXT("AvidScript compile result did not include WASM bytecode."),
			TEXT("load a manifest-backed WASM artifact before applying reload"),
			OutResult);
		return false;
	}

	FAvidScriptWasmReloadResult RuntimeResult;
	const bool bHadLiveRuntime = ReloadSession.IsLiveLoaded();
	const bool bRuntimeSucceeded = bHadLiveRuntime
		? ReloadSession.ReloadModule(
			CompileResult.Bytecode.GetData(),
			CompileResult.Bytecode.Num(),
			CompileResult.Manifest,
			RuntimeResult)
		: ReloadSession.LoadInitialModule(
			CompileResult.Bytecode.GetData(),
			CompileResult.Bytecode.Num(),
			CompileResult.Manifest,
			RuntimeResult);

	OutResult.RuntimeResult = RuntimeResult;
	if (!bRuntimeSucceeded)
	{
		CopyAvidScriptReloadRuntimeFailure(RuntimeResult, OutResult);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.bApplied = true;
	OutResult.bInitialLoad = !bHadLiveRuntime;
	OutResult.bReload = bHadLiveRuntime;
	OutResult.Status = bHadLiveRuntime
		? EAvidScriptEditorReloadApplyStatus::AppliedReload
		: EAvidScriptEditorReloadApplyStatus::AppliedInitialLoad;
	return true;
}
