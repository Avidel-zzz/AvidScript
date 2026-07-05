#pragma once

#include "AvidScriptEditorReloadService.h"

#include "CoreMinimal.h"

enum class EAvidScriptEditorCommandStatus : uint8
{
	Unknown,
	CompileFailed,
	GeneratedOnly,
	ReloadApplied,
	ReloadFailed
};

struct FAvidScriptEditorCommandConfig
{
	FAvidScriptEditorCompileConfig CompileConfig;
	FAvidScriptWasmReloadSession* ReloadSession = nullptr;
};

struct FAvidScriptEditorCommandResult
{
	bool bSucceeded = false;
	bool bReloadApplied = false;
	EAvidScriptEditorCommandStatus Status = EAvidScriptEditorCommandStatus::Unknown;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FAvidScriptEditorCompileResult CompileResult;
	FAvidScriptEditorReloadApplyResult ReloadApplyResult;
};

class FAvidScriptEditorCommandService
{
public:
	static bool ApplyEvaluatedCompileResult(
		const FAvidScriptEditorCompileResult& CompileResult,
		FAvidScriptWasmReloadSession& ReloadSession,
		FAvidScriptEditorCommandResult& OutResult);

	static bool CompileAndApply(
		const FAvidScriptEditorCommandConfig& Config,
		FAvidScriptEditorCommandResult& OutResult);
};
