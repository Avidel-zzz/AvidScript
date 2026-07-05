#include "AvidScriptEditorCommandService.h"

namespace
{
void SetAvidScriptCommandFailure(
	const EAvidScriptEditorCommandStatus Status,
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorCommandResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.bReloadApplied = false;
	OutResult.Status = Status;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}

EAvidScriptEditorCommandStatus MapAvidScriptCommandFailureStatus(
	const FAvidScriptEditorCompileResult& CompileResult,
	const FAvidScriptEditorReloadApplyResult& ApplyResult)
{
	if (!CompileResult.bSucceeded || CompileResult.Status == EAvidScriptEditorCompileStatus::FailedDiagnostics ||
		CompileResult.Status == EAvidScriptEditorCompileStatus::FailedInvocation ||
		CompileResult.Status == EAvidScriptEditorCompileStatus::FailedManifest)
	{
		return EAvidScriptEditorCommandStatus::CompileFailed;
	}

	return EAvidScriptEditorCommandStatus::ReloadFailed;
}
} // namespace

bool FAvidScriptEditorCommandService::ApplyEvaluatedCompileResult(
	const FAvidScriptEditorCompileResult& CompileResult,
	FAvidScriptWasmReloadSession& ReloadSession,
	FAvidScriptEditorCommandResult& OutResult)
{
	OutResult = FAvidScriptEditorCommandResult();
	OutResult.CompileResult = CompileResult;

	FAvidScriptEditorReloadApplyResult ApplyResult;
	if (!FAvidScriptEditorReloadService::ApplyCompileResult(CompileResult, ReloadSession, ApplyResult))
	{
		OutResult.ReloadApplyResult = ApplyResult;
		SetAvidScriptCommandFailure(
			MapAvidScriptCommandFailureStatus(CompileResult, ApplyResult),
			ApplyResult.ErrorCategory,
			ApplyResult.ErrorMessage,
			ApplyResult.NextAction,
			OutResult);
		return false;
	}

	OutResult.ReloadApplyResult = ApplyResult;
	OutResult.bSucceeded = true;
	if (ApplyResult.Status == EAvidScriptEditorReloadApplyStatus::SkippedGeneratedOnly)
	{
		OutResult.Status = EAvidScriptEditorCommandStatus::GeneratedOnly;
		OutResult.NextAction = ApplyResult.NextAction;
		return true;
	}

	OutResult.bReloadApplied = ApplyResult.bApplied;
	OutResult.Status = EAvidScriptEditorCommandStatus::ReloadApplied;
	return true;
}

bool FAvidScriptEditorCommandService::CompileAndApply(
	const FAvidScriptEditorCommandConfig& Config,
	FAvidScriptEditorCommandResult& OutResult)
{
	OutResult = FAvidScriptEditorCommandResult();
	if (Config.ReloadSession == nullptr)
	{
		SetAvidScriptCommandFailure(
			EAvidScriptEditorCommandStatus::CompileFailed,
			TEXT("reload_session_missing"),
			TEXT("AvidScript command requires a reload session."),
			TEXT("provide a caller-owned FAvidScriptWasmReloadSession before running the command"),
			OutResult);
		return false;
	}

	FAvidScriptEditorCompileResult CompileResult;
	FAvidScriptEditorCompileService::Compile(Config.CompileConfig, CompileResult);
	return ApplyEvaluatedCompileResult(CompileResult, *Config.ReloadSession, OutResult);
}
