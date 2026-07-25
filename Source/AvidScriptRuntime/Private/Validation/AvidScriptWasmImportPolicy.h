#pragma once

#include "CoreMinimal.h"

struct FAvidScriptWasmModuleLayout;
struct FAvidScriptWasmReloadManifest;

struct FAvidScriptWasmImportContractResult
{
	FString ErrorCategory;
	FString ErrorDetails;
	FString NextAction;
};

bool ValidateAvidScriptWasmImportContract(
	const FAvidScriptWasmModuleLayout& WasmLayout,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmImportContractResult& OutResult);

bool InspectAndValidateAvidScriptWasmImportContract(
	TConstArrayView<uint8> Bytecode,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmImportContractResult& OutResult);
