#include "AvidScriptVmBackend.h"

#ifndef AVIDSCRIPT_WITH_WAMR
#define AVIDSCRIPT_WITH_WAMR 0
#endif

namespace
{
void SetFactoryError(FAvidScriptVmError& OutError, const TCHAR* Category, const FString& Details)
{
	OutError.Reset();
	OutError.Category = Category;
	OutError.Details = Details;
}
}

TUniquePtr<IAvidScriptVmBackend> CreateAvidScriptVmBackend(
	const FAvidScriptVmBackendSelection& Selection,
	FAvidScriptVmError& OutError)
{
	OutError.Reset();
	if (Selection.BackendKind != EAvidScriptVmBackendKind::Wamr)
	{
		SetFactoryError(
			OutError,
			TEXT("backend_unavailable"),
			TEXT("The requested VM backend is unavailable in this build."));
		return nullptr;
	}

#if !AVIDSCRIPT_WITH_WAMR
	SetFactoryError(
		OutError,
		TEXT("backend_unavailable"),
		TEXT("The WAMR VM backend is unavailable in this build."));
	return nullptr;
#else
	const bool bRequestsInterpreter = Selection.ExecutionMode == EAvidScriptVmExecutionMode::Interpreter;
	const bool bCanFallbackToInterpreter = Selection.bAllowFallback
		&& (Selection.ExecutionMode == EAvidScriptVmExecutionMode::Auto
			|| Selection.ExecutionMode == EAvidScriptVmExecutionMode::Aot
			|| Selection.ExecutionMode == EAvidScriptVmExecutionMode::Jit);
	if (!bRequestsInterpreter && !bCanFallbackToInterpreter)
	{
		SetFactoryError(
			OutError,
			TEXT("execution_mode_unavailable"),
			TEXT("The requested WAMR execution mode is unavailable in this build."));
		return nullptr;
	}

	if (Selection.ArtifactFormat != EAvidScriptVmArtifactFormat::WasmBytecode
		&& !Selection.bAllowFallback)
	{
		SetFactoryError(
			OutError,
			TEXT("artifact_format_unavailable"),
			TEXT("The WAMR interpreter accepts canonical WASM bytecode only."));
		return nullptr;
	}

	return CreateAvidScriptWamrBackend();
#endif
}
