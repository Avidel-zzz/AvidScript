#pragma once

#include "AvidScriptFrontendInvoker.h"
#include "AvidScriptWasmReload.h"

#include "CoreMinimal.h"

enum class EAvidScriptEditorCompileStatus : uint8
{
	Unknown,
	SucceededGeneratedOnly,
	SucceededReloadable,
	FailedDiagnostics,
	FailedInvocation,
	FailedManifest
};

struct FAvidScriptEditorCompileConfig
{
	FAvidScriptFrontendInvocationConfig InvocationConfig;
	FString ManifestPathOverride;
};

struct FAvidScriptEditorCompileResult
{
	bool bSucceeded = false;
	bool bReloadable = false;
	bool bManifestLoadAttempted = false;
	EAvidScriptEditorCompileStatus Status = EAvidScriptEditorCompileStatus::Unknown;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString ManifestPath;
	FAvidScriptFrontendInvocationResult InvocationResult;
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
};

class FAvidScriptEditorCompileService
{
public:
	static bool Compile(
		const FAvidScriptEditorCompileConfig& Config,
		FAvidScriptEditorCompileResult& OutResult);

	static bool EvaluateInvocationResult(
		const FAvidScriptFrontendInvocationResult& InvocationResult,
		FAvidScriptEditorCompileResult& OutResult);

	static bool EvaluateInvocationResult(
		const FAvidScriptFrontendInvocationResult& InvocationResult,
		const FString& ManifestPathOverride,
		FAvidScriptEditorCompileResult& OutResult);
};
