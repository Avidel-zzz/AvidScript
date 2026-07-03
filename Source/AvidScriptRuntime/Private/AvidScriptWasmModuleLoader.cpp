#include "AvidScriptWasmModuleLoader.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWasmModuleLoader, Log, All);

namespace
{
void PrepareLoadResult(FAvidScriptWasmModuleLoadResult& OutResult, const FString& ModulePath)
{
	OutResult = FAvidScriptWasmModuleLoadResult();
	OutResult.ModulePath = ModulePath;
}

void SetLoadFailure(
	FAvidScriptWasmModuleLoadResult& OutResult,
	const FString& Category,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ByteSize = 0;
	OutResult.ErrorCategory = Category;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript WASM module load error | path=%s | category=%s | details=%s | next=%s"),
		OutResult.ModulePath.IsEmpty() ? TEXT("<none>") : *OutResult.ModulePath,
		*Category,
		*Details,
		*NextAction);

	UE_LOG(LogAvidScriptWasmModuleLoader, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}
} // namespace

bool FAvidScriptWasmModuleLoader::LoadFromFile(
	const FString& ModulePath,
	TArray<uint8>& OutBytecode,
	FAvidScriptWasmModuleLoadResult& OutResult)
{
	OutBytecode.Reset();
	PrepareLoadResult(OutResult, ModulePath);

	if (ModulePath.IsEmpty())
	{
		SetLoadFailure(
			OutResult,
			TEXT("module_path_invalid"),
			TEXT("module path is empty"),
			TEXT("provide an absolute or project-relative .wasm file path"));
		return false;
	}

	if (!FPaths::FileExists(ModulePath))
	{
		SetLoadFailure(
			OutResult,
			TEXT("module_file_missing"),
			FString::Printf(TEXT("file does not exist: %s"), *ModulePath),
			TEXT("build or copy the WASM module before loading it"));
		return false;
	}

	if (!FFileHelper::LoadFileToArray(OutBytecode, *ModulePath))
	{
		OutBytecode.Reset();
		SetLoadFailure(
			OutResult,
			TEXT("module_file_read_failed"),
			FString::Printf(TEXT("failed to read file: %s"), *ModulePath),
			TEXT("verify file permissions and that no external process is still writing the module"));
		return false;
	}

	if (OutBytecode.IsEmpty())
	{
		OutBytecode.Reset();
		SetLoadFailure(
			OutResult,
			TEXT("module_file_empty"),
			FString::Printf(TEXT("file is empty: %s"), *ModulePath),
			TEXT("rebuild the WASM module and retry after the file is fully written"));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ByteSize = OutBytecode.Num();
	return true;
}
