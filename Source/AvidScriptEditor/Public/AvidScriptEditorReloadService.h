#pragma once

#include "AvidScriptEditorCompileService.h"

#include "CoreMinimal.h"

enum class EAvidScriptEditorReloadApplyStatus : uint8
{
	Unknown,
	SkippedGeneratedOnly,
	AppliedInitialLoad,
	AppliedReload,
	RejectedCompileResult,
	FailedRuntime
};

struct FAvidScriptEditorReloadApplyResult
{
	bool bSucceeded = false;
	bool bApplied = false;
	bool bInitialLoad = false;
	bool bReload = false;
	EAvidScriptEditorReloadApplyStatus Status = EAvidScriptEditorReloadApplyStatus::Unknown;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FAvidScriptWasmReloadResult RuntimeResult;
};

class FAvidScriptEditorReloadService
{
public:
	static bool ApplyCompileResult(
		const FAvidScriptEditorCompileResult& CompileResult,
		FAvidScriptWasmReloadSession& ReloadSession,
		FAvidScriptEditorReloadApplyResult& OutResult);
};
